# Review fix round 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the review's crash, data-loss, and gate holes that this round owns: one size-capped regular-file reader and a confined glTF walk, dockspace modals that cannot starve each other, a gizmo abandon that keeps an undo step, a sticky play Stop, a cook queue that cannot spin or terminate, mesh-index and artifact-identity checks, one process JobSystem with a safe shutdown, CI gates that fail closed, a server that exits when `--remote` or `--report` did not do what was asked, a held frame 0 that has world pixels, and a Servitor CLI whose json envelope stays on stdout and whose owner pid is opt-in.

**Architecture:** Each fix is a shared helper plus a test that fails when a sibling keeps the old pattern. The file reader and the URI check live in ArcaneCore so Runtime, Client, Developer, and the editor program all call one function. glTF node walk becomes an explicit heap stack in the pipeline importer. Editor frame glue that the test exe does not link (DrawModals, the gizmo else-branch, the Save button, EndFrame's plugin poll) is pinned by a source scan; the decision itself is a pure function or a CommandStack / PlaySession method the test exe already links. Those host-visible edits also get a Servitor check on a headless host this round launched. ProcessContext keeps a `weak_ptr<JobSystem>` and each live Runtime holds the `shared_ptr`. `Runtime.cpp` resolves the worker count and passes it in; one mutex covers the expired-check and the mint, so overlapping Runtime construction cannot mint twice, and the count is read only when the weak has expired. Servitor json and ndjson write one envelope on stdout and leave stderr empty, including usage refusals and a verbose posted call. `--client` and `--owner-pid` reach the descriptor only when the operator typed them; `HostSession` writes both keys and refreshes them. An omitted `--owner-pid` stores 0 and watches nobody. The 600 s idle backstop is unchanged. A held runtime frame runs `TransformPropagationSystem` then `BoundsSystem` and does not call `RunLoop::Advance`.

**Tech Stack:** C++23, premake5, MSBuild (VS 2026), Catch2 v3, NRI (D3D12 + Vulkan), cgltf, enkiTS, Astra ECS, PowerShell 5.1, bash, GitHub Actions.

**Spec:** The findings and the fix sentences are `D:\dev\starworks\arcane-review-2026-10-16\SCORECARD.md` (and `scorecard.json` beside it), synthesized against review commit `47c5f136`. Lane reports and critiques under that directory are the per-finding wording. The Servitor screenshot and CLI contract is `D:\dev\starworks\arcane-servitor-fix\diagnosis.md`, written against Arcane-f5 `309aa06e`. Where a diagnosis sentence defaults the owner pid to servitor's parent, this plan wins: `--owner-pid` is opt-in and `0` means no watch. Where this plan and the scorecard disagree on a line number, this tree wins; the drift table below is the crosswalk. Do not re-open ranks 8, 9, 10, 12, or 15.

**Revision:** 2026-10-17, anchored to `D:\dev\starworks\Arcane-fixes` branch `fix/review-round-1` at `ab8b7af9` (F5 plan 1 and the D3D12 debug layer are already on this main). Every `file:line` below was re-read on this tree. Revised the same day against `.superpowers/review-fix-1/plan-review.md` (3 Critical, 6 Important); the task bodies below are that revision. Revised again the same day against `.superpowers/review-fix-1/plan-rereview.md`: Tasks 5, 6, 7, 17, and 24 drive the changed path, Task 21 allows `--yes` on `quit --client`, Task 20 pre-scans argv for the format, and Task 4's visited check is a hash set. Scorecard lines that moved:

| Finding | Scorecard / diagnosis cite | This tree |
|---|---|---|
| H1 error modal, unsaved OpenPopup, create gate, ignored Stop | `EditorAppFrame.cpp:3950`, `:4028`, `:4001`, `:4064` | error modal `:3969-3988`; create gate `:4020-4021` (the starvation comment is `:4002-4019`); unsaved `OpenPopup` `:4047-4049`; Save's ignored `Stop` `:4083-4085` |
| H2 gizmo Cancel, Q | `:1498`, Q at `:1154`, `CommandStack.cpp:210` | Select/Q sets `m_gizmoEnabled = false` at `EditorAppFrame.cpp:1216`; `gizmoActive` `:1422-1424`; Cancel else-branch `:1557-1560`; `CommandStack::Cancel` is `Source/Editor/ArcaneEdit/src/Arcane/Edit/CommandStack.cpp:210-219` |
| M01 Stop, ClearSceneReferences | `PlayMode.cpp:173-175`, `EditorAppScene.cpp:131` | `Stop` is `:132-177`; `m_mode = Edit` is still `:175`, before `return ok`; `ClearSceneReferences` ignores `Stop` at `EditorAppScene.cpp:133-135` |
| H3 `NriGraphContext` tellg | `:634` | `ShaderBytecode` tellg is `NriGraphContext.cpp:656` |
| H3 `InputActions` | `:55` | ifstream `:57`, tellg `:60` |
| H3 `Assets.cpp` | `:61` | `ReadFileBytes` starts `:61`, tellg `:66` |
| M23 `--remote` Start, `--report` | `ServerApp.cpp:207-208`, `:51-55` | Start failure continues at `:209-210`; `Finish` still returns the passed `exitCode` after a failed `WriteTo` at `:54-59` |
| Hold branch | diagnosis `RuntimeFrame.cpp:229` | Hold is `:229-237` and returns with no transform or bounds pass |
| InfoQueue1 warn | named file only | the warn is `DeviceCreationD3D12.cpp:765-774` |
| Scene dumper depth | "caps at 64" | `SceneCommands.cpp:45-46`, `kMaxWalkDepth = 64`, because the JSON dump recurses. The glTF walk does not share that cap (Task 4). |

`kGamePluginABIVersion` is 61 (`Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp:10`). This plan edits neither that file nor `PluginABI.hpp`. `Descriptor` grows two optional JSON keys; missing keys already read as the struct defaults (`Descriptor.hpp:44-47`). That struct is not a game-plugin by-value type. If an executor is forced to bump, the number is the next ABI after `origin/main` at execution time, read from `AbiVersion.hpp` first. Never a number written here.

## Global Constraints

- **Worktree:** `D:\dev\starworks\Arcane-fixes`, branch `fix/review-round-1`. Never push, never force-push, never merge to `main`. The plan file is the only document this round adds. Do not edit `ThirdParty/`.
- **One heavy build at a time.** Before every build or test-target rebuild, refuse to start if `cl`, `link`, `MSBuild`, or `ArcaneTests` is already running for another tree:

```powershell
$busy = Get-Process -Name cl,link,MSBuild,ArcaneTests -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ($_.Path -notlike 'D:\dev\starworks\Arcane-fixes\*') }
if ($busy) {
    $busy | Format-Table Id, ProcessName, Path -AutoSize
    throw 'another worktree holds cl, link, MSBuild, or ArcaneTests'
}
```

  A process whose path is under this worktree is this round's own build. Wait for it. Do not start a second one.

- **Desk cap.** The user is at the desk. Every build is Debug, four MSBuild nodes, four CL workers, below-normal priority. `scripts\ci\build.ps1` maps `-Jobs` to `msbuild /m` and does not set `_CL_` or the priority; the caller does:

```powershell
$env:CI = $null
$env:ARCANE_BUILD_MACHINE = $null
$env:_CL_ = '/MP4'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
& D:\dev\starworks\Arcane-fixes\scripts\ci\build.ps1 Debug -Jobs 4
if ($LASTEXITCODE -ne 0) { throw "build.ps1 exited $LASTEXITCODE" }
```

  Logs: `logs\build-{generate,engine,referencegame,tests}.log`. A new `.cpp`/`.hpp` under a module `src/` needs no premake edit (`build/modules.lua:397` globs). A new `Tests/ArcaneTests/src/*.cpp` is picked up by `premake5.lua:1316-1318` on the next generate, which `build.ps1` always runs. Editor headers are visible to tests (`premake5.lua:913`). `PlayMode.cpp` and `CookQueue.cpp` are compiled into ArcaneTests (`premake5.lua:1348`, `:1683`). `EditorAppFrame.cpp`, `EditorAppScene.cpp`, and `EditorApp.cpp` are not.

- **RED rebuilds the test target first.** A step marked RED runs the desk-cap preamble, then builds `ArcaneTests` (the full `build.ps1 Debug -Jobs 4` when an engine module changed; `msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m:4` only when the change is a test file or a header the test exe compiles directly and no other `cl`/`link` is running). Then it runs the filter from the exe directory and quotes the failure it expects. A green run on a RED step is a wrong test. Stop. Do not "fix" it by weakening the assertion.

- **Tests.** Unset `CI` and `ARCANE_BUILD_MACHINE` in the same shell before every test process. Run from the exe directory and record the seed banner:

```powershell
$env:CI = $null
$env:ARCANE_BUILD_MACHINE = $null
Set-Location D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "<filter>"
```

  Copy the `Randomness seeded to:` line into the commit message. Reproduce a failure with `--rng-seed N`. Suite gate before each task's commit: `powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-fixes\scripts\run-tests.ps1 Debug` (it unsets those two variables itself; still unset them in the parent shell). GPU cases are `[gpu]` and are not part of that gate. A green `[gpu][golden]` Catch2 case is not a host render.

- **Goldens.** Tasks 18 and 19 run `powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-fixes\scripts\golden-gate.ps1` and require 14 lanes (`scripts/golden-gate.ps1:326`, "fourteen today"), each `diffCount` 0. Do not `--bless`. Do not bless through Servitor. If a lane diffs, stop and report. World lanes (`runtime-scene`, `f3-cull-blend`) must stay 0. The Hold-path change is frame 0 of a held host; goldens step the sim. A diff means this assumption is wrong.

- **Servitor, for every host-visible change.** Read `.claude/skills/arcane-servitor/SKILL.md` before the first call. Binary: `D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe` from this tree, beside the host exe from the same Debug build. Loop: `launch <kind> --headless` (runtime adds `-- --fixed-time 0`), then `--client grok-review-r1 --host <kind> wait settled`, then the command under test, then `quit`. A step whose host is specified to exit before ready does not `wait settled`. When that launch returns 7 the process has already exited; do not `quit` it. When it returns 8 (`launch.timeout`) the process is still running; `quit --pid` that pid and the step has failed. Branch on the exit code and write it down: 0 ok, 2 usage, 3 permission, 4 no host, 5 busy (retry), 6 command failed, 7 host died, 8 check failed. Launch a headless host of your own. Do not drive an editor the user has open. `quit` every host this round launched that is still running, including when the check fails. Tasks 5, 6, 7, 17, 18, 19, 20, 22, and 24 record every command, its exit code, and the artifact path in the commit message. Task 21 launches no host: the omitted-client `PlanLaunch` and the status/quit selection are its Catch2 cases. Until Task 21, `launch` and `status` still refuse `--client` (exit 2); do not pass it to those two. Pass `--client grok-review-r1` on every other host call. From Task 21 on, `launch` and `status` take it too.

- **Style.** `/MD`, no `/fp:fast`, MKS, ASCII comments, UTF-8 without BOM. A new Core function is `ARC_CORE_API` (`<Arcane/Core/Api.hpp>`). Do not add a cvar for a cap that is a format limit.

- **Every commit message ends with exactly these two trailer lines.** The F5 plan's trailer omits `(1M context)`. Do not copy that trailer.

```
Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

- **Sibling sweep.** The review's repeated failure is a fix that lands in one copy. Every task greps, then either routes the hit through the helper or lists it in the commit body under `Left as-is:` with the reason. `ThirdParty/` is always left as-is.

## Review Focus

These are the five inputs most likely to bite that a neighboring test would miss. Each pinning test is written in the task named.

1. **A directory, a non-regular file, and a file over the cap must refuse without throwing, on this desk and on libstdc++.** MSVC's `ifstream` already fails on a directory, so a test that only opens a directory goes green against today's `MeshImporter.cpp:35-52`. The cap is the desk-side RED. `ReadRegularFile` uses `is_regular_file` plus `file_size`, never `tellg`. Size `>` cap returns `nullopt`. Size `==` cap returns the bytes. A 0-byte regular file returns an empty vector, not `nullopt` (callers that treat empty as missing keep that policy at the call site: `Assets.cpp` `ReadFileBytes`). Allocation stays inside `try`. Pinned in Task 1 (`"ReadRegularFile refuses a directory, a non-regular path, and a file over the cap, and returns an empty vector for an empty file"`).
2. **A glTF buffer URI that is absolute, or that contains `..` and names a real file outside the asset directory, is refused by both readers. A sibling file in the asset directory still loads. A 4096-deep child chain imports, and the baked mesh's vertices and AABB are that leaf's world transform applied to its local positions.** `cgltf_validate` rejects cycles, not depth. The scene dumper's 64 (`SceneCommands.cpp:46`) exists because its JSON dump recurses; the importer's product is a flat vector, so the walk does not borrow that cap. The visited check is `std::unordered_set<const cgltf_node*>::insert`, so the walk stays linear in the node count. `ImportMesh` returns `MeshImportResult` (`MeshImporter.hpp:65`), a baked `ImportedMesh`, not a node-mesh pair or a leaf matrix. Pinned in Task 3 (`"both external-buffer readers refuse an absolute uri and a .. uri, and load a sibling"`) and Task 4 (`"a 4096-deep node chain bakes parent-times-local positions"`).
3. **Abandon commits when every snapshotted component still exists, and restores survivors then cancels when any is gone. `Cancel` itself still does not revert.** Inspector and `EditGesture` call `Cancel` and depend on that. Q mid-drag and deselect are the all-exist path (the mid-drag pose stays, Ctrl+Z reverts it). Delete of one target in a multi-select is the restore path (the dead component is a no-op, each survivor gets its `before` bytes, no undo entry). Pinned in Task 6 (`"Abandon commits a live target and restores a survivor when one target is gone; Cancel still does not revert"`).
4. **One JobSystem per process is a weak slot, not a process-lifetime strong ref.** `Test::Process()` lives for the Catch2 process, and the suite order is random. `SettingsBindingsTest.cpp` sets `jobs.workerThreads` to 1 and checks a new Runtime's `WorkerCount()` against `JobSystem(1)`. A strong ref latched by whichever Runtime ran first makes that check order-dependent. `ApplyEngineRungAndResolveWorkers()` stays in `Runtime.cpp` (it is file-local, `:131`). The Runtime ctor passes that count into `AcquireJobSystem`. The slot mints only when the weak has expired, and the mutex covers the expired-check and the mint together so two overlapping constructions cannot each mint. Destroying one of two Runtimes must leave the other's `Submit` runnable. `~JobSystem` calls `WaitforAllAndShutdown` before it drops the adapter, then nulls the executor so a `shared_ptr` kept past destruction runs inline. Pinned in Task 12 (`"two Runtimes share one JobSystem and the survivor can Submit"`) and by the existing worker-count cases staying green in the suite gate. Task 11 pins the destructor order (`"~JobSystem shuts the scheduler down before it resets the adapter"`).
5. **A held headless frame 0 has world pixels and has not stepped physics or `frameCount`. A json usage refusal and a verbose posted call leave stderr empty, including a `ParseArgs` failure after `--json` or `--format json`.** An omitted `--owner-pid` stores 0 and watches nobody. A deliberately supplied dead owner pid exits. `quit --client` does not match a descriptor whose client is empty, and more than one match is confirmed with `--yes` without `--all`. Defaulting the owner pid to servitor's parent kills the host, because agent shells exit when the call returns. The 600 s idle backstop stays. Pinned in Task 18 (`"Hold presents world transform and bounds without advancing frameCount"`), Task 20 (`"json usage and a verbose posted call leave stderr empty"`), Task 21 (`"omitted --client is not forwarded, and status and quit select the named client"`), and Task 22 (`"omitted --owner-pid is zero, and a dead owner pid exits"`).

## Out of scope

Do not implement any of these. A task that starts one is wrong; stop.

- Scorecard ranks 8 (one cvar mutation path, M13), 9 (physics stepping, M21), 10 (CreateProcessW spawn, M06/M08), 12 (rename-everywhere, M05), and 15 / H4 (physics decoupling).
- The other Mediums and Lows not named in Tasks 1–24. L05's "warn-once drops" is not this round except the one InfoQueue1 line in Task 24; the sibling sweep there lists the other warns.
- Unifying the two `.arcart` codecs. They stay mirrored. Task 9 adds the same index check to both.
- An ABI bump, a `PluginABI.hpp` edit, or any edit under `ThirdParty/`.
- Defaulting `--owner-pid` to servitor's parent, to the launching shell, or to any pid the operator did not pass.
- `--bless`, including through Servitor.
- Measuring the hosted-CI Dist baseline by setting `CI` or `ARCANE_BUILD_MACHINE` on this desk. Task 14 records an exemption for that one lane instead of a guessed integer.
- A new fuzzer. Task 15 only replays `scripts/run-fuzz.sh regress` and `scripts/namespace-compile-fail.sh`.

## File map

| File | Task | Responsibility |
|---|---|---|
| `Source/Runtime/ArcaneCore/src/Arcane/IO/ReadRegularFile.hpp`, `.cpp` (new) | 1 | `ReadRegularFile`, `kMaxRegularFileBytes`. |
| `Tests/ArcaneTests/src/ReadRegularFileTest.cpp` (new) | 1 | Review Focus 1. |
| Production `tellg` whole-file readers listed in Task 2 | 2 | Call `ReadRegularFile`. Grep guard. |
| `Source/Runtime/ArcaneCore/src/Arcane/IO/PathConfine.hpp` (new) | 3 | `PathIsInsideDirectory`. |
| `MeshImporter.cpp`, `ArtifactReader.cpp` | 3, 4, 9 | URI confinement, iterative walk, index check. |
| `Tests/ArcaneTests/src/ExternalBufferPathTest.cpp`, `MeshImportDeepTreeTest.cpp` (new) | 3, 4 | Review Focus 2. |
| `Source/Programs/ArcaneEditor/src/App/ModalGate.hpp` (new) | 5 | `ModalAllowed`. |
| `EditorAppFrame.cpp` DrawModals, `Remote/ModalProbeCommand.cpp` (new) | 5 | Open only the allowed popup. `editor.modal.probe` arms an error and the create dialog. |
| `Remote/GizmoProbeCommand.cpp` (new) | 6 | `editor.gizmo.probe` arms the restore-path drag. |
| `Remote/PlayRestoreProbeCommand.cpp` (new) | 7 | `editor.play.fail-next-restore` arms the sticky Stop. |
| `CommandStack.hpp`, `CommandStack.cpp` | 6 | `Abandon`. `Cancel` unchanged. |
| `EditorAppFrame.cpp` gizmo else-branch | 6 | Call `Abandon`. |
| `Tests/ArcaneTests/src/CommandStackTest.cpp` | 6 | Review Focus 3. |
| `PlayMode.hpp`, `PlayMode.cpp` | 7 | Sticky `Stop`, `FailNextRestoreForTesting`. |
| `EditorApp.hpp`, `EditorAppScene.cpp`, `EditorAppFrame.cpp`, `EditorAppProject.cpp` | 7 | Abort Save and `ClearSceneReferences`; skip `Poll` while playing. |
| `Tests/ArcaneTests/src/EditorPlayModeTest.cpp` | 7 | Sticky Stop. |
| `CookQueue.cpp`, `CookQueue.hpp` | 8 | try/catch, deadline wait. |
| `Tests/ArcaneTests/src/CookQueueTest.cpp` | 8 | A throwing importer delivers a failure and clears `m_running`. |
| `ArtifactReader.cpp`, `ArtifactFormat.cpp` | 9 | Refuse `index >= vertexCount`. |
| `ArtifactStore.hpp`, `ArtifactStore.cpp`, `CookSession.cpp` | 10 | Delete every non-current `.arcart`; validate on the up-to-date path. |
| `JobSystem.cpp`, `ArcaneWorkScheduler.hpp` | 11 | Shutdown order, then null the executor. |
| `Tests/ArcaneTests/src/JobSystemSubmitTest.cpp` | 11 | Review Focus 4's order pin. |
| `ProcessContext.hpp`, `ProcessContext.cpp`, `Runtime.cpp`, `Runtime.hpp` | 12 | Weak slot. Runtime passes the resolved worker count. `Jobs()` and `TaskExecutor()` dereference the `shared_ptr`. |
| `Tests/ArcaneTests/src/RuntimeTest.cpp` | 12 | Review Focus 4's gate. |
| `.github/workflows/ci.yml`, `scripts/ci/ArcaneCi.psm1`, `scripts/ci/tests/test-arcane-ci.ps1` | 13 | Uint32 seeds computed in a shell step. |
| `scripts/check-baselines.ps1`, `scripts/check-baselines.py` (new), `scripts/automation-baselines.json`, `scripts/baseline-exemptions.json` (new), `scripts/run-tests.sh`, `scripts/ci/ArcaneCi.psm1` | 14 | Missing baseline fails; no summary fails; POSIX runs the checker. |
| `.github/workflows/ci.yml` | 15, 16 | Fuzz regress, compile-fail, PR images do not push. |
| `scripts/ci/ci-image.sh` | 16 | `PUBLISH=true` only off a PR. |
| `ServerApp.cpp` | 17, 22 | `--remote` Start failure exits 2; `--report` write failure cannot exit 0. Task 22 assigns `rc.ownerPid` at the `:191` config. |
| `RuntimeFrame.hpp`, `RuntimeFrame.cpp` | 18 | `PresentDerivedState` on Hold. |
| `Tests/ArcaneTests/src/RuntimeHoldPresentTest.cpp` (new) | 18 | Review Focus 5's Hold pin. |
| Servitor screenshot evidence | 19 | World pixels at frame 0. Golden gate. |
| `Servitor/Format.cpp`, `Client.cpp`, `Commands.cpp` | 20 | json/ndjson stderr empty. |
| `Servitor/Verbs.cpp`, `Launch.cpp`, `Discover.cpp`, `Descriptor.hpp`, `Descriptor.cpp`, `HostSession.hpp`, `HostSession.cpp`, `HostConfig.hpp`, `ServerConfig.hpp` | 21, 22 | An explicit `--client` is forwarded and the host writes it. `quit --client` and `status --client` select. Owner pid is opt-in. Idle stays 600. |
| `RuntimeApp.cpp`, `EditorAppFrame.cpp`, `ServerApp.cpp` | 21, 22 | Copy the parsed client (Task 21) and owner pid (Task 22) onto `HostSession::Config`. Idle copy drops the headless conjunct. |
| `.claude/skills/arcane-servitor/SKILL.md`, `CLAUDE.md`, `AGENTS.md` | 23 | Wrappers and the Servitor sections. |
| `DeviceCreationD3D12.cpp` | 24 | InfoQueue1 warn once per process. The guard does not return; queue creation still runs. |

---

### Task 1: One size-capped regular-file reader

**Files:**
- Create: `Source/Runtime/ArcaneCore/src/Arcane/IO/ReadRegularFile.hpp`
- Create: `Source/Runtime/ArcaneCore/src/Arcane/IO/ReadRegularFile.cpp`
- Test: `Tests/ArcaneTests/src/ReadRegularFileTest.cpp`

**Interfaces:**
- Consumes: `std::filesystem::is_regular_file`, `std::filesystem::file_size`. No `tellg`.
- Produces:
  - `inline constexpr std::uint64_t kMaxRegularFileBytes = 1ull << 30;` (1 GiB). A cooked mesh or a shader blob in this tree is far under it. `LLONG_MAX` is over it.
  - `ARC_CORE_API std::optional<std::vector<std::byte>> ReadRegularFile(const std::filesystem::path& path, std::uint64_t maxBytes = kMaxRegularFileBytes);`
  - Contract: `nullopt` when the path is missing, not a regular file, `file_size` fails, `file_size > maxBytes`, the open fails, or the allocation throws. A regular file of size 0 returns an empty vector. A regular file of size `== maxBytes` returns those bytes. Never throws.

- [ ] **Step 1: Write the failing test.**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>
#include <fstream>
#include <filesystem>

static std::filesystem::path Scratch(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "arcane-read-regular";
    std::filesystem::create_directories(dir);
    return dir / name;
}

TEST_CASE("ReadRegularFile refuses a directory, a non-regular path, and a file over the cap, and returns an empty vector for an empty file",
          "[io][read]") {
    const auto dir = Scratch("a-directory");
    std::filesystem::create_directory(dir);
    CHECK_FALSE(Arcane::ReadRegularFile(dir).has_value());

    const auto empty = Scratch("empty.bin");
    { std::ofstream out(empty, std::ios::binary); }
    const auto emptyBytes = Arcane::ReadRegularFile(empty);
    REQUIRE(emptyBytes.has_value());
    CHECK(emptyBytes->empty());

    const auto small = Scratch("sixteen.bin");
    {
        std::ofstream out(small, std::ios::binary);
        out << "0123456789abcdef";
    }
    CHECK_FALSE(Arcane::ReadRegularFile(small, 15).has_value());
    const auto exact = Arcane::ReadRegularFile(small, 16);
    REQUIRE(exact.has_value());
    CHECK(exact->size() == 16);

    CHECK_FALSE(Arcane::ReadRegularFile(Scratch("no-such-file")).has_value());
}
```

- [ ] **Step 2: RED.** Desk-cap preamble, then `build.ps1 Debug -Jobs 4` (the new test cpp needs a generate). Run from the exe dir: `.\ArcaneTests.exe "[io][read]"`. Expected FAIL: the build log contains `error C1083` or `C2039` naming `ReadRegularFile.hpp` / `ReadRegularFile` (the header does not exist yet). Record the seed only if the exe actually started. A passing run means the test did not compile in. Stop.

- [ ] **Step 3: Implement the reader.**

`ReadRegularFile.hpp`:

```cpp
#pragma once
#include <Arcane/Core/Api.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace Arcane {
inline constexpr std::uint64_t kMaxRegularFileBytes = 1ull << 30;
[[nodiscard]] ARC_CORE_API std::optional<std::vector<std::byte>>
ReadRegularFile(const std::filesystem::path& path,
                std::uint64_t maxBytes = kMaxRegularFileBytes);
}
```

`ReadRegularFile.cpp`:

```cpp
#include <Arcane/IO/ReadRegularFile.hpp>
#include <fstream>
#include <system_error>

namespace Arcane {
std::optional<std::vector<std::byte>>
ReadRegularFile(const std::filesystem::path& path, std::uint64_t maxBytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return std::nullopt;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz > maxBytes)
        return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::vector<std::byte> out;
    try {
        out.resize(static_cast<std::size_t>(sz));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!out.empty() &&
        !in.read(reinterpret_cast<char*>(out.data()),
                 static_cast<std::streamsize>(out.size())))
        return std::nullopt;
    return out;
}
}
```

The body contains `is_regular_file` and `file_size` and does not contain `tellg`. Add that as a second `CHECK` in the test by reading this `.cpp` through `Arcane::Test::RepoRoot()` (`Tests/ArcaneTests/src/Helpers/ConstantScan.hpp`) and asserting those three facts. That pin is what fails on a later edit that puts `tellg` back, including on MSVC where a directory already refuses.

- [ ] **Step 4: GREEN.** Same preamble and filter. Expected: both cases PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** `tellg` under `Source/` (exclude nothing yet; Task 2 does the retarget). Record the list in the commit body. Do not retarget in this task. Do not edit `ThirdParty/`.

- [ ] **Step 6: Commit.** Suite gate `scripts\run-tests.ps1 Debug` first.

```
Add a size-capped regular-file reader in ArcaneCore.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 2: Every whole-file tellg reader calls the helper

**Files:**
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp:35-52` (`ReadWholeFileBytes`)
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookSession.cpp:161-178`
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.cpp:294-311` and `:520-540` (the second `tellg` is `:532`)
- Modify: `Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactReader.cpp:224-253` (`ReadWholeFile` becomes a call to `ReadRegularFile`) and `ReadFilePrefix` at `:273` (add `is_regular_file` before the fixed-size read; it does not use `tellg` and must not start)
- Modify: `Source/Runtime/ArcaneAssets/src/Arcane/Assets/Assets.cpp:61-74` (`ReadFileBytes`). Keep today's call-site policy: a `nullopt` or an empty vector is "missing". Do not make `ReadRegularFile` collapse those two.
- Modify: `Source/Programs/ArcaneEditor/src/Project/MeshImportWave.cpp:22-42`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppProject.cpp:1467-1484` (`ReadWholeGltfSource`)
- Modify: `Source/Client/ArcaneInput/src/Arcane/Input/InputActions.cpp:54-68`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp:640-671` (`ShaderBytecode`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriDiagnostics.cpp:394-405` (`ReadShaderBlob`)
- Modify: `Source/Programs/Servitor/src/Servitor/Client.cpp:741-760` (`ReadLogFile`): `is_regular_file` before `tellg`. This one reads a tail, not a whole-file vector, so it does not call `ReadRegularFile`. A directory returns the existing unreadable error.
- Test: `Tests/ArcaneTests/src/ReadRegularFileTest.cpp` (append the grep guard)

**Interfaces:**
- Consumes: `Arcane::ReadRegularFile` from Task 1.
- Produces: no new signature. Local helpers that returned `optional<vector<byte>>` return `ReadRegularFile(path)` directly.

- [ ] **Step 1: Write the grep guard, then RED.** Append:

```cpp
TEST_CASE("production sources do not size a whole file with tellg", "[io][read]") {
    const auto root = Arcane::Test::RepoRoot();
    const std::vector<std::string> allowed = {
        // tail read, guarded by is_regular_file in this same task
        "Source/Programs/Servitor/src/Servitor/Client.cpp",
    };
    // Walk root / "Source" for .cpp/.hpp. Compare the path relative to root.
    // Search the comment-stripped source. Fail on a code occurrence of "tellg"
    // whose relative path is not in `allowed`.
}
```

  Implement the walk with `std::filesystem::recursive_directory_iterator` on `root / "Source"`. `RepoRoot()` is the reference project's parent (`ConstantScan.hpp:34`), so the iterator yields absolute paths. Compare `std::filesystem::relative(entry.path(), root).generic_string()` to the relative `allowed` entries (`"Source/Programs/Servitor/src/Servitor/Client.cpp"`). Do not compare `path.generic_string()` to that relative entry. Skip any relative path containing `"/ThirdParty/"`.

  Search code, not retained comments. Read the file, blank `"..."` and `'...'` literals, drop a trailing `//` comment the way `StripLiteralsAndComments` (`ConstantScan.hpp:51`) does, and drop `/* */` blocks, then search the stripped text for `tellg`. A comment that still says `tellg` is not a hit. The allowed tail read is the code at `Client.cpp:754` (`in.tellg()`), which survives the strip. On a hit whose relative path is not in `allowed`, `FAIL` with that relative path.

  Desk-cap preamble, rebuild ArcaneTests, run `"[io][read]"`. Expected FAIL: Catch2 reports `MeshImporter.cpp`, `CookSession.cpp`, `ArtifactFormat.cpp`, `ArtifactReader.cpp`, `Assets.cpp`, `MeshImportWave.cpp`, `EditorAppProject.cpp`, `InputActions.cpp`, `NriGraphContext.cpp`, `NriDiagnostics.cpp`. `ReadLogFile`'s code hit fails too until Step 3 adds it to `allowed` after the `is_regular_file` guard. Record the seed.

- [ ] **Step 2: Retarget.** Each whole-file helper becomes `return Arcane::ReadRegularFile(path);` and drops its local `ifstream` / `tellg` / `vector` ctor. `ArtifactReader::ReadWholeFile` is that one-line wrapper so `ReadClientExternalBuffers` (`:1026`) stays source-compatible. `ReadFilePrefix` returns `nullopt` when `!is_regular_file` before it sizes the fixed buffer. `ReadLogFile` returns `unreadable()` when `!is_regular_file`. `ShaderBytecode` and `ReadShaderBlob` copy the `vector<byte>` into `vector<uint8_t>` after a successful read (the helper's element type is `byte`).

- [ ] **Step 3: Add `Client.cpp` to `allowed` only after `ReadLogFile` contains `is_regular_file`.** The guard stays red if any other `Source/` `tellg` remains.

- [ ] **Step 4: GREEN.** Same filter. Expected PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `tellg` in `Tests/` and `ThirdParty/`. List every hit under `Left as-is:`. Test helpers that read a file the test just wrote stay. `ThirdParty/Astra`, `imgui-node-editor`, and spdlog tests stay. Do not edit them. Confirm `Source/` outside `allowed` is clean.

- [ ] **Step 6: Commit.** Suite gate first.

```
Route whole-file reads through ReadRegularFile.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 3: Confine glTF buffer URIs to the asset directory

**Files:**
- Create: `Source/Runtime/ArcaneCore/src/Arcane/IO/PathConfine.hpp`
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp:66-71` (`DecodeUriToPath`) and `:753-789` (`ReadExternalBuffers`, join at `:780`)
- Modify: `Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactReader.cpp:991-1032` (`ReadClientExternalBuffers`, join at `:1026-1027`)
- Test: `Tests/ArcaneTests/src/ExternalBufferPathTest.cpp` (existing directory cases) and a pipeline-side case beside the mesh importer tests

**Interfaces:**
- Consumes: `ReadRegularFile` (Task 2 already makes both readers use it).
- Produces, header-only in ArcaneCore:

```cpp
namespace Arcane {
// True when `candidate` is a regular file whose weakly_canonical path is
// strictly inside weakly_canonical(`directory`). `candidate` may be absolute:
// the asset directory is absolute, and `base / relative` is absolute too.
// False for empty, when canonicalization fails, when the candidate is not a
// regular file, and when the relative path from the canonical directory to
// the canonical candidate is empty or starts with "..".
[[nodiscard]] inline bool PathIsInsideDirectory(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate);
}
```

  Both readers, after the existing `data:` skip and after percent-decode, compute `decoded` and refuse an absolute decoded URI before any join. `operator/` replaces its left side when the right side `is_absolute()`, so checking the joined path cannot tell an absolute URI from a relative one. The join itself is `MeshImporter.cpp:780` (`baseDir` is `:766`) and `ArtifactReader.cpp:1026-1027`.

```cpp
if (decoded.is_absolute())
    return std::nullopt;
const fs::path bufferPath = baseDir / decoded;
if (!Arcane::PathIsInsideDirectory(baseDir, bufferPath))
    return std::nullopt;
```

  `decoded` is the path `DecodeUriToPath` / `DecodeUriPercentEscapes` already returns. Order stays: raw `data:` check, then decode, then reject an absolute decoded URI, then join, then confine, then read. `uri == nullptr` / missing uri stays skipped. The helper accepts the joined path, which is absolute whenever `baseDir` is, when that canonical path lies strictly inside the canonical base directory.

- [ ] **Step 1: Write the failing tests.** Extend `ExternalBufferPathTest.cpp` with a temp asset dir that contains `ok.bin`, plus a file outside it. A glTF whose buffer uri is that sibling loads. A glTF whose uri is the outside file's absolute path returns `nullopt` from `ReadClientExternalBuffers`. A glTF whose uri is `../<outside-name>` returns `nullopt`. Mirror the same three uris through `Arcane::AssetPipeline::ReadExternalBuffers` in a `[pipeline]` case (that function is already the pipeline entry the cook calls; if it is not declared in the header, the test includes `MeshImporter.hpp` and calls the public function that `CookSession` uses to fill external buffers — read `MeshImporter.hpp` and call that public function, do not copy the join). Also `CHECK` `PathIsInsideDirectory` itself: `base / "ok.bin"` (absolute, strictly inside) is true; the outside file's absolute path is false; `base / "."` and `base / ".."` are false.

- [ ] **Step 2: RED.** Rebuild ArcaneTests (engine header: full `build.ps1`). Run `"[pipeline]"` filtered to the new case name plus the existing external-buffer case. Expected FAIL: the absolute and `../` cases return a byte vector today (`MeshImporter.cpp:780` joins with no confinement). The sibling case already passes; if it fails, the fixture is wrong. Stop.

- [ ] **Step 3: Implement `PathIsInsideDirectory` and both call sites.** `weakly_canonical` on both sides inside `std::error_code` overloads. Compare with `path::lexically_relative` and reject when the relative path is empty or starts with `..`. Require `is_regular_file` on the candidate. Do not reject a candidate for being absolute. Reject `decoded.is_absolute()` before the join, at both readers.

- [ ] **Step 4: GREEN.** Same filter. Expected PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `parent_path()` joined with a uri or a guid-derived relative path under `Source/`. `EditorAppProject.cpp` guid joins (the review cited `:1864` on the old tree; re-find by grepping `parent_path()` in that file) get the same helper when they concatenate a relative string onto a content root, or a `Left as-is:` line when the string is an engine-minted guid with no `..` segment and the grep shows a reject of `..` already. Do not leave an unconfined user-string join unmarked.

- [ ] **Step 6: Commit.** Suite gate first.

```
Confine glTF buffer URIs to the asset directory.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 4: Iterative glTF node walk

**Files:**
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp:204-212` (`WalkNodeTree`) and the two call sites `:428` and `:434`
- Test: `Tests/ArcaneTests/src/MeshImportDeepTreeTest.cpp` (new)

**Interfaces:**
- Consumes: `cgltf_node`, `LocalTransform` (`MeshImporter.cpp:190`).
- Produces: the same `WalkNodeTree` signature, body replaced. Depth-first, parent before children, `world = parentWorld * LocalTransform(node)`, one output pair per node that has a mesh. An explicit `std::vector` stack. A visited `std::unordered_set<const cgltf_node*>`: membership is `insert`, which keeps the walk linear in the node count. An index bitset would need `cgltf_data` (`cgltf.h` exposes a flat `nodes` array) and a signature change; the pointer set fits the signature `WalkNodeTree` already has. No depth cap of 64. The parsed node array is the bound.

- [ ] **Step 1: Write the failing test.** The RED is a source pin. Start the scan after `WalkNodeTree`'s opening brace. The declaration at `MeshImporter.cpp:204` contains the text `WalkNodeTree(`, so a window that begins at `void WalkNodeTree` stays red after the recursive call is gone.

```cpp
TEST_CASE("WalkNodeTree does not call itself", "[pipeline][mesh]") {
    const auto text = ReadRepoFile("Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/MeshImporter.cpp");
    const auto fn = text.find("void WalkNodeTree");
    REQUIRE(fn != std::string::npos);
    const auto brace = text.find('{', fn);
    const auto next = text.find("struct RawSection", brace);
    REQUIRE(brace != std::string::npos);
    REQUIRE(next != std::string::npos);
    REQUIRE(next > brace);
    const auto body = text.substr(brace + 1, next - (brace + 1));
    CHECK(body.find("WalkNodeTree(") == std::string::npos);
    CHECK(body.find("std::vector") != std::string::npos);
    CHECK(body.find("std::unordered_set") != std::string::npos);
    CHECK(body.find("std::find") == std::string::npos);
}
```

  `struct RawSection` is the next declaration (`MeshImporter.cpp:218`). The body today contains the recursive call at `:211` and does not contain `std::unordered_set`. The signature text before the brace is not part of `body`. `std::find` over a visited vector is the quadratic check this pin refuses: the stack may stay a `std::vector`, the visited container may not be scanned with `std::find`.

  In the same file, write the behavior case and do not run it in Step 2. Build a glTF JSON in a temp file: one mesh primitive, a triangle with positions `(0,0,0)`, `(1,0,0)`, `(0,1,0)` and no `NORMAL`, and a chain of 4096 nodes where node `i` is the only child of node `i-1` and only the leaf references the mesh. Each node's translation is `(1, 0, 0)`. Call `ImportMesh` (`MeshImporter.hpp:85`) with the bytes and empty external buffers. `ImportMesh` returns `MeshImportResult` (`MeshImporter.hpp:65`): `refusal` empty, `mesh` engaged. It does not return node-mesh refs or a leaf matrix. The bake writes world positions into `MeshArtifactVertex::px/py/pz` (`MeshImporter.cpp:495` and `:560`) and the AABB (`:707-709`). Require the set of `px` to be `{4096, 4097}` (local x `0` and `1`, plus 4096 parent translations of `1`), `aabbMin[0] == 4096`, `aabbMax[0] == 4097`, `aabbMin[1] == 0`, and `aabbMax[1] == 1`, within a few ulps. `meshopt` may reorder vertices; assert the set and the bounds, not vertex order. Name the case `"a 4096-deep node chain bakes parent-times-local positions"`.

- [ ] **Step 2: RED.** Rebuild and run `.\ArcaneTests.exe "WalkNodeTree does not call itself"`. Expected FAIL: `body.find("WalkNodeTree(") == npos` is false because the body still contains the call at `:211`, and `std::unordered_set` is absent. Do not run the 4096-chain case here (a deep chain that overflows never returns a Catch2 line). Record the seed.

- [ ] **Step 3: Replace the body.**

```cpp
void WalkNodeTree(const cgltf_node& root, const glm::mat4& parentWorld,
                   std::vector<std::pair<const cgltf_mesh*, glm::mat4>>& out)
{
    struct Frame { const cgltf_node* node; glm::mat4 parent; };
    std::vector<Frame> stack;
    stack.push_back({ &root, parentWorld });
    std::unordered_set<const cgltf_node*> seen;
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (!seen.insert(frame.node).second)
            continue;
        const glm::mat4 world = frame.parent * LocalTransform(*frame.node);
        if (frame.node->mesh != nullptr)
            out.emplace_back(frame.node->mesh, world);
        for (cgltf_size i = frame.node->children_count; i > 0; --i)
            stack.push_back({ frame.node->children[i - 1], world });
    }
}
```

  `MeshImporter.cpp` includes `<algorithm>`, `<cstring>`, `<fstream>`, `<limits>`, `<string>`, and `<utility>` (`:13-18`) and does not include `<unordered_set>`. Add that include next to them. Pushing children right-to-left keeps the left-to-right order the recursive walk produced, so `nested.gltf` stays observable. The existing nested test is the order regression. Run it in Step 4. `insert` is the visited check; a `std::find` over a growing vector makes the 4096-chain quadratic and fails the pin.

- [ ] **Step 4: GREEN.** Run `"WalkNodeTree does not call itself"`, `"a 4096-deep node chain bakes parent-times-local positions"`, and the existing nested-gltf case. Expected PASS. The chain case's `px` set and AABB are the assertion; a green pin with an unchecked import is not this step. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `WalkNodeTree` and other `cgltf_node` recursion under `Source/`. `SceneCommands.cpp`'s walk is already iterative and capped at 64 for a different reason (`:45-46`). Leave it. List any other recursive node walk.

- [ ] **Step 6: Commit.** Suite gate first.

```
Walk glTF nodes on an explicit stack.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 5: Dockspace modals are mutually exclusive, errors first

**Files:**
- Create: `Source/Programs/ArcaneEditor/src/App/ModalGate.hpp`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp:3964-4049` (`DrawModals`) and the register call at `:2132`
- Create: `Source/Programs/ArcaneEditor/src/Remote/ModalProbeCommand.cpp` (new, `#if !defined(ARC_BUILD_DIST)`)
- Test: `Tests/ArcaneTests/src/ModalGateTest.cpp` (new)

**Interfaces:**
- Produces:

```cpp
namespace Arcane::Editor {
enum class ModalKind : std::uint8_t { None, Error, UnsavedScene, CreateAsset };
[[nodiscard]] inline ModalKind ModalAllowed(bool errorQueued,
                                            bool sceneIntentParked,
                                            bool createOpen) {
    if (errorQueued) return ModalKind::Error;
    if (sceneIntentParked) return ModalKind::UnsavedScene;
    if (createOpen) return ModalKind::CreateAsset;
    return ModalKind::None;
}
}
```

  `DrawModals` computes `const ModalKind allowed = ModalAllowed(m_modalErrors.Front() != nullptr, pending != SceneIntent::None, m_createDialog.open);` once, after `pending` is read (`:3994`). The error block runs only when `allowed == Error`. The create block's existing condition (`:4020`) becomes `allowed == CreateAsset`. The unsaved `OpenPopup` at `:4047-4049` moves inside `if (allowed == UnsavedScene)`. `BeginPopupModal("Unsaved Scene")` also runs only in that branch, so a parked intent cannot re-arm while an error is up. The create dialog's state is left untouched when it loses, which is what the comment at `:4016-4019` already requires. At the end of `DrawModals`, store `allowed` in a latch the probe command reads.
- Produces `editor.modal.probe`, a Dist-stripped remote command registered beside `RegisterSelectionCommand` (`EditorAppFrame.cpp:2132`, inside the `#if !defined(ARC_BUILD_DIST)` block that ends at `:2134`). Same shape as `editor.selection.get` (`SelectionCommand.cpp:115-137`): `ExecThread::Main`, `needsFrame` false. The command holds an `EditorApp*` installed at that site and calls `EditorApp::ArmModalProbe` / `DismissModalProbe` / `ModalProbeKind`, which are the only code that touch `m_modalErrors` and `m_createDialog`. `DrainFrame` (`:507`) runs before `DrawModals` (`:659`), so `op=drawn` reports the latch the previous frame stored. Args are one object key `op`:
  - `arm`: `m_modalErrors.Push("Probe Error", ...)` and `BeginCreateAsset` with `CreateAssetKind::Material` (`EditorAppFrame.cpp:3719`). ReferenceProject is open, so this takes the project branch and sets `m_createDialog.open`. If `CurrentProject()` is null, `BeginCreateAsset` pushes `"Create Failed"` instead (`:3722-3728`); the command then returns an error and the step stops. Reply includes `createOpen: true`.
  - `dismiss`: `m_modalErrors.Pop()`, the same pop the OK button performs (`:3983`). Leaves `m_createDialog.open` as it was.
  - `drawn`: reply `kind` is the latch (`Error`, `UnsavedScene`, `CreateAsset`, or `None`).

- [ ] **Step 1: Write the failing test.**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "App/ModalGate.hpp"

TEST_CASE("errors beat unsaved, unsaved beats create, none is none", "[editor][modal]") {
    using Arcane::Editor::ModalAllowed;
    using Arcane::Editor::ModalKind;
    CHECK(ModalAllowed(true, true, true) == ModalKind::Error);
    CHECK(ModalAllowed(true, false, false) == ModalKind::Error);
    CHECK(ModalAllowed(false, true, true) == ModalKind::UnsavedScene);
    CHECK(ModalAllowed(false, false, true) == ModalKind::CreateAsset);
    CHECK(ModalAllowed(false, false, false) == ModalKind::None);
}
```

  And a source pin over `EditorAppFrame.cpp` `DrawModals`: the unsaved `OpenPopup("Unsaved Scene")` call's position is greater than a `ModalAllowed(` call in the same function, and the function contains `allowed == ModalKind::UnsavedScene` (or the enumerator text) before that `OpenPopup`. The same pin requires the text `editor.modal.probe` in that cpp (the register call). Before the edit the pin fails because `ModalAllowed` and `editor.modal.probe` are absent.

- [ ] **Step 2: RED.** Rebuild ArcaneTests. Run `"[editor][modal]"`. Expected FAIL: `C1083` on `ModalGate.hpp`, or once the header exists and the pin is in the test, `ModalAllowed` is absent from `DrawModals`. Do not accept a pass. Record the seed if the exe ran.

- [ ] **Step 3: Add the header, gate `DrawModals`, and add the probe.** Do not change the error modal's button behavior. Do not change `SceneSession` intent parking. The probe cpp is editor-program source (the module glob picks it up; no premake edit). Register it only on the successful-Start path next to `RegisterSelectionCommand`. Store the latch at the end of `DrawModals` after `allowed` is known.

- [ ] **Step 4: GREEN.** Same filter PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `OpenPopup(` under `Source/Programs/ArcaneEditor`. Every dockspace-level modal in `DrawModals` is covered. A panel-local `OpenPopup` is `Left as-is:` with the window name, because ImGui's one-popup rule is per window stack level and a panel popup does not share the dockspace level. List them.

- [ ] **Step 6: Servitor.** Drive the two competing dockspace modals. An idle frame never enters the gate: `DrawModals` (`EditorAppFrame.cpp:3964`) re-arms every popup, and two of them at the dockspace level close each other (`:4002-4015`). Headless still calls `DrawModals` (`:659`) and `screenshot window` composites that chrome. `launch` takes no `--client` until Task 21. `call` is the verb (`Verbs.cpp:157`).

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$dir = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1'
$err = Join-Path $dir 'task5-error.png'
$create = Join-Path $dir 'task5-create.png'
$drawn1 = Join-Path $dir 'task5-drawn-error.json'
$drawn2 = Join-Path $dir 'task5-drawn-create.json'
$logs = Join-Path $dir 'task5-logs.json'
& $s --project ReferenceProject launch editor --headless
& $s --project ReferenceProject --client grok-review-r1 --host editor wait settled
& $s --project ReferenceProject --client grok-review-r1 --host editor call editor.modal.probe op=arm
& $s --project ReferenceProject --client grok-review-r1 --host editor screenshot window --out $err
& $s --project ReferenceProject --client grok-review-r1 --host editor call editor.modal.probe op=drawn | Out-File -Encoding utf8 $drawn1
& $s --project ReferenceProject --client grok-review-r1 --host editor call editor.modal.probe op=dismiss
& $s --project ReferenceProject --client grok-review-r1 --host editor screenshot window --out $create
& $s --project ReferenceProject --client grok-review-r1 --host editor call editor.modal.probe op=drawn | Out-File -Encoding utf8 $drawn2
& $s --project ReferenceProject --client grok-review-r1 --host editor logs --level warn | Out-File -Encoding utf8 $logs
& $s --project ReferenceProject --client grok-review-r1 --host editor quit
```

  `DrainFrame` (`:507`) runs before `DrawModals` (`:659`), so `op=drawn` is the latch the frame that just presented stored. The screenshot between `arm` and the first `drawn` is that frame, and the same pairing follows `dismiss`. Record every command, its exit code, and the five artifact paths. Expected: launch, wait, `arm`, both screenshots, both `drawn` calls, `dismiss`, and quit exit 0. `arm`'s reply has `createOpen` true. `$drawn1` has `data.kind` `Error` (the error modal is up, the create dialog is waiting). `$drawn2` has `data.kind` `CreateAsset` (dismiss popped the error, and the create dialog `BeginCreateAsset` left open is the one that draws). Both PNGs exist and are non-empty: `$err` is the error modal, `$create` is the create dialog. `host.state` is not the assertion; a headless editor does not publish `modal`. Do not `play`, `stop`, or `quit` an editor this step did not launch. Quit even when a check fails.

- [ ] **Step 7: Commit.** Suite gate first. The message includes the Step 6 exit codes and artifact paths.

```
Gate dockspace modals so errors open first.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 6: Gizmo abandonment commits or restores

**Files:**
- Modify: `Source/Editor/ArcaneEdit/src/Arcane/Edit/CommandStack.hpp` (declare `Abandon` next to `Cancel`)
- Modify: `Source/Editor/ArcaneEdit/src/Arcane/Edit/CommandStack.cpp:210-219` (add `Abandon` after `Cancel`; do not change `Cancel`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp:1551-1561` and the register call at `:2132`
- Create: `Source/Programs/ArcaneEditor/src/Remote/GizmoProbeCommand.cpp` (new, `#if !defined(ARC_BUILD_DIST)`)
- Test: `Tests/ArcaneTests/src/CommandStackTest.cpp` (the abandon case at `:151-209` stays; add the new case beside it)

**Interfaces:**
- Consumes: `Commit` (`:162`), `Cancel` (`:210`), `Pending::before` (`SnapshotComponent` `:148-160`), `ComponentEditCommand::Restore` (`ComponentEditCommand.cpp:40-54`, no-op when the component is gone). `Restore` is private on the command. `Abandon` does not call it from outside. It writes survivors back by constructing the same command the stack already constructs and calling `Undo()` on that temporary, which calls `Restore(m_before)`.
- Produces: `void CommandStack::Abandon(TransactionId owner);`
  - Owner mismatch: return, same as `Cancel`.
  - `m_pending` empty and `m_pendingGeneric` empty: `Cancel(owner)`.
  - Every `Pending`'s `GetComponentByHash` returns non-null, and `m_pendingGeneric` is empty: `Commit(owner)`.
  - Otherwise: for each `Pending` whose component still exists, build a `ComponentEditCommand` with that `before` and an empty after, call `Undo()`, then `Cancel(owner)`. No undo entry. A generic pending command in the mixed case is dropped by `Cancel` (it was never applied as its own inverse; gizmo drags do not push generic commands). Document that in a comment on `Abandon`.
- Produces `editor.gizmo.probe`, Dist-stripped, registered beside `RegisterSelectionCommand` (`EditorAppFrame.cpp:2132`). It holds an `EditorApp*` installed at that site and calls `EditorApp::ArmGizmoAbandonProbe()`, which is the only code that touches `m_selection`, `m_undo`, and `m_gizmoDrag`. The command runs inside `DrainFrame` (`:507`), which returns before `FrameInput` (`:624`) calls `UpdateGizmoInteraction` (`:1388`). `ArmGizmoAbandonProbe` arms the drag and does not call `Abandon` or `Cancel`. The same frame's else-branch (`:1551-1561`) is what calls `Abandon`. Arming:
  1. `CreateEntityInScene` (`EntityOps.hpp:67`) twice under the live scene root, then `RenameEntity` (`:143`) to `GizmoProbeA` and `GizmoProbeB`. Set both `Transform` positions to `0`. A refused create (no live scene root) returns `host.unavailable` and the step stops.
  2. Select both. Copy the press path (`EditorAppFrame.cpp:1463-1490`): `Begin("Gizmo")`, `SnapshotComponent` both, write both `position.x = 5`, set `m_gizmoDrag.active` and `m_gizmoDrag.txn`.
  3. `DeleteEntities` (`EntityOps.hpp:102`) on B only, then clear the selection so `HasSelection()` is false. `gizmoActive` (`:1422-1424`) is then false while `m_gizmoDrag.active` is still true, which is the else-branch. B's component is gone, so `Abandon` restores A to the snapped `0` and pushes no undo entry. Reply: `survivor` `GizmoProbeA`, `beforeX` `0`, `sentinelX` `5`.

- [ ] **Step 1: Write the failing test** in `CommandStackTest.cpp`, using the same registry/stack setup as the case at `:151`.

```cpp
TEST_CASE("Abandon commits a live target and restores a survivor when one target is gone; Cancel still does not revert",
          "[edit]") {
    // Arrange two entities with Transform position.x = 0, Begin, Snapshot both,
    // write both position.x = 5.
    // Cancel(txn) leaves both at 5 and CanUndo() false.  (existing contract)
    // A second gesture: Snapshot both, write both to 5, Abandon(txn).
    // Both stay at 5, CanUndo() true, Undo() returns both to 0.
    // A third gesture: Snapshot both, write both to 7, destroy entity B, Abandon(txn).
    // Entity A is back at its snapped before (0 if the third gesture snapped 0),
    // CanUndo() false.
}
```

  Fill the arrange from the existing test's types (`Transform`, `Begin`, `SnapshotComponent`). Do not change the existing case's assertions.

- [ ] **Step 2: RED.** Rebuild ArcaneTests (ArcaneEdit is a module the tests link; full `build.ps1`). Run `.\ArcaneTests.exe "Abandon commits a live target"`. Expected FAIL: `C2039` `'Abandon' is not a member of 'CommandStack'`. Record nothing if it did not link.

- [ ] **Step 3: Implement `Abandon`, switch the else-branch, and add the probe.** At `EditorAppFrame.cpp:1557-1560` replace `m_undo->Cancel(m_gizmoDrag.txn)` with `m_undo->Abandon(m_gizmoDrag.txn)`. Leave the mouse-up `Commit` at `:1543-1547` alone. Add a source pin in the same test file: `EditorAppFrame.cpp`'s `UpdateGizmoInteraction` contains `Abandon(` and the else-branch of `m_gizmoDrag.active` does not contain `Cancel(`. The pin also requires the text `editor.gizmo.probe`. The probe cpp is editor-program source; the module glob picks it up. Register it on the successful-Start path next to `RegisterSelectionCommand`. A headless editor has no viewport press (`:1446` starts a drag only on an in-viewport left press), so the probe fills the same `GizmoDrag` fields the press path fills and then removes one target. Q and deselect leave every component alive, and that path commits (the pose stays); the Catch2 case owns it. The probe is the restore path, which is the one whose transform returns to the snapshot.

- [ ] **Step 4: GREEN.** The new case and `"Commit is the safe close for an ABANDONED gesture"` both PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `->Cancel(` under `Source/`. Inspector / `EditGesture` call sites stay on `Cancel` and are listed as `Left as-is:` because their abandon path is `Commit` when the edit should stick (the existing test). Only the gizmo else-branch switches to `Abandon`.

- [ ] **Step 6: Servitor.** Start a drag, let the else-branch abandon it, and read the survivor. `entity get` is `name:<unique name> --component Transform` (`Verbs.cpp:178`). The get is a later frame's `DrainFrame`, so it runs after the arm frame's `UpdateGizmoInteraction` has called `Abandon`. `launch` takes no `--client` until Task 21.

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$dir = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1'
$arm = Join-Path $dir 'task6-arm.json'
$got = Join-Path $dir 'task6-entity.json'
$logs = Join-Path $dir 'task6-logs.json'
& $s --project ReferenceProject launch editor --headless
& $s --project ReferenceProject --client grok-review-r1 --host editor wait settled
& $s --project ReferenceProject --client grok-review-r1 --host editor call editor.gizmo.probe | Out-File -Encoding utf8 $arm
& $s --project ReferenceProject --client grok-review-r1 --host editor entity get name:GizmoProbeA --component Transform | Out-File -Encoding utf8 $got
& $s --project ReferenceProject --client grok-review-r1 --host editor logs --level warn | Out-File -Encoding utf8 $logs
& $s --project ReferenceProject --client grok-review-r1 --host editor quit
```

  Record every command, its exit code, and the three json paths. Expected: launch, wait, `call`, `entity get`, and quit exit 0. `$arm` has `data.survivor` `GizmoProbeA`, `data.beforeX` `0`, `data.sentinelX` `5`. `$got` is `entity.get`'s `data.components.Transform.position.x` (`EcsCommands.hpp:38`), and that x equals `beforeX` and is not `sentinelX`: `Abandon` restored the survivor. Do not drive an editor the user has open. Quit even when a check fails.

- [ ] **Step 7: Commit.** Suite gate first. The message includes the Step 6 exit codes and artifact paths.

```
Commit or restore an abandoned gizmo drag.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 7: A failed Stop stays in Play, and Save and Clear abort

**Files:**
- Modify: `Source/Programs/ArcaneEditor/src/App/PlayMode.hpp` (declare `FailNextRestoreForTesting` on `PlaySession`)
- Modify: `Source/Programs/ArcaneEditor/src/App/PlayMode.cpp:132-177`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorApp.hpp` (`ClearSceneReferences` at the declaration near `:2282`, `void` becomes `bool`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppScene.cpp:126-166`, `DoNewScene` `:169`, `DoOpenScene` `:199`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp:4083-4087`, the `Poll` call in `EndFrame` (`:4935-4946`), and the register call at `:2132`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppProject.cpp` `ResetPerProjectState` (the `ClearSceneReferences` call near `:2217`)
- Create: `Source/Programs/ArcaneEditor/src/Remote/PlayRestoreProbeCommand.cpp` (new, `#if !defined(ARC_BUILD_DIST)`)
- Test: `Tests/ArcaneTests/src/EditorPlayModeTest.cpp`

**Interfaces:**
- Produces:
  - `void PlaySession::FailNextRestoreForTesting();` sets a private bool.
  - `Stop`: the embedded-server teardown (`:140-155`) stays where it is. After the restore attempt, `SetPaused(true)` runs on both outcomes. On `!ok`, do not assign `m_mode`, do not clear `m_snapshot`, return `false`. On success, assign `EditorMode::Edit` and return `true`. The test flag, when set, is cleared and forces `ok = false` without calling `RestoreRegistry` or `LoadStatePrimary`, so the play mutation remains. That simulates the transactional failure `RestoreRegistry` already has (`Runtime.cpp:531-564`).
  - `bool EditorApp::ClearSceneReferences(std::string reason);` When `wasPlaying` and `Stop` returns false: push a modal whose text contains `could not be stopped`, return false, and do not call `AfterPlayFlip`, `m_serverProcess.Stop`, `m_selection.Clear`, `m_undo->Clear`, or anything below `:135`. When `Stop` returns true, the rest of the function is unchanged and it returns true.
  - Save button (`:4083`): `if (wasPlaying && !m_play.Stop(...)) { push the same modal; return; }` before `AfterPlayFlip` and before `DoSaveScene`.
  - `DoNewScene` and `DoOpenScene`: if `ClearSceneReferences` returns false, return false and do not `ResetRegistry` / load. `DoOpenScene` reads the file before the clear today; on a false clear it returns false without applying that file.
  - `EndFrame`: `if (!InPlayMode()) m_plugin->Poll();` so a sticky Play cannot hot-reload over the open snapshot. `StartModuleRebuild` already refuses while playing (`EditorAppProject.cpp:2902-2905`); leave it.
  - `editor.play.fail-next-restore`, Dist-stripped, registered beside `RegisterSelectionCommand` (`EditorAppFrame.cpp:2132`). No args. It holds an `EditorApp*` and calls `EditorApp::ArmFailNextRestore()`, which calls `m_play.FailNextRestoreForTesting()`. That method is not a host flag; this command is how Servitor arms it. `play.stop` already maps a false `editorStop` to `play.refused` / `the editor could not restore the edit snapshot on Stop` (`PlayCommands.cpp:240-245`), client exit 6.

- [ ] **Step 1: Write the failing test** in `EditorPlayModeTest.cpp`, next to the existing play/stop round trip.

```cpp
TEST_CASE("a failed Stop stays in Play and a second Stop restores", "[editor][play]") {
    // Play, set the primary sprite sortingLayer (or Transform) to a sentinel
    // the snapshot does not have, FailNextRestoreForTesting(), Stop returns false,
    // IsPlaying() is true, the sentinel is still there.
    // Stop again with no flag: returns true, IsPlaying() is false, the sentinel is gone.
}
```

  Use the fixture the file already uses to enter Play. Add a source pin: `PlayMode.cpp`'s `Stop` contains `if (!ok)` before `m_mode = EditorMode::Edit`, and `EditorAppFrame.cpp` contains `could not be stopped`, the `Poll` call is guarded by `!InPlayMode()`, and the text `editor.play.fail-next-restore` is present.

- [ ] **Step 2: RED.** Rebuild ArcaneTests (`PlayMode.cpp` is in the test exe, so a test-only edit can use `/t:ArcaneTests` after the preamble; the first implementation edit of `PlayMode.hpp` needs that same project). Run the new case. Expected FAIL: `FailNextRestoreForTesting` is not a member, or the source pin does not find `if (!ok)`. Record the seed if it ran.

- [ ] **Step 3: Implement the contract above, and add the probe.** Do not reorder the server teardown. The residual, written as a comment on the early-failure return: a failed `Stop` has already dropped the embedded server and switched the primary to Standalone; the snapshot is intact so the next `Stop` retries the restore, and `IsPlaying()` stays true so Save and `Poll` stay blocked. Register `editor.play.fail-next-restore` on the successful-Start path next to `RegisterSelectionCommand`. The probe cpp is editor-program source; the module glob picks it up. The source pin also requires the text `editor.play.fail-next-restore`.

- [ ] **Step 4: GREEN.** The new case and the existing play round-trip PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `.Stop(` on `m_play` under `Source/Programs/ArcaneEditor`. Every call that continues into a scene write or a registry reset checks the bool. A call that only queries state is `Left as-is:`.

- [ ] **Step 6: Servitor.** Drive a failed restore. A successful Stop returns the editor to Edit and never enters the `!ok` branch this task adds. `FailNextRestoreForTesting` forces that branch. The editor process stays up: the fix keeps Play and returns false. The server's failed Start is Task 17. This task lands first, and `Stop` does not exit the editor. `could not be stopped` is the modal Save (`EditorAppFrame.cpp:4083`) and `ClearSceneReferences` push when they see a false `Stop`; this step does not press Save, so that sentence is not the witness. The witness is `play.stop`'s `play.refused` envelope and `host.state` still `playing`. `launch` takes no `--client` until Task 21. The verb is `stats frame` (`Verbs.cpp:134`). Read `host.state` on that envelope (`Protocol.cpp:178`).

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$dir = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1'
& $s --project ReferenceProject launch editor --headless
& $s --project ReferenceProject --client grok-review-r1 --host editor wait settled
& $s --project ReferenceProject --client grok-review-r1 --host editor play
& $s --project ReferenceProject --client grok-review-r1 --host editor stats frame | Out-File -Encoding utf8 "$dir\task7-playing.json"
& $s --project ReferenceProject --client grok-review-r1 --host editor call editor.play.fail-next-restore
$stop = Join-Path $dir 'task7-stop.json'
& $s --project ReferenceProject --json --client grok-review-r1 --host editor stop | Out-File -Encoding utf8 $stop
& $s --project ReferenceProject --client grok-review-r1 --host editor stats frame | Out-File -Encoding utf8 "$dir\task7-still-playing.json"
& $s --project ReferenceProject --client grok-review-r1 --host editor stop
& $s --project ReferenceProject --client grok-review-r1 --host editor stats frame | Out-File -Encoding utf8 "$dir\task7-restored.json"
& $s --project ReferenceProject --client grok-review-r1 --host editor quit
```

  Record every command, its exit code, and the four json paths. Expected: launch, wait, play, the first `stats frame`, the probe `call`, the second `stop`, the third `stats frame`, and quit exit 0. The first `stop` exits 6. `$stop` has `ok` false and `errors[0].code` `play.refused`, and `errors[0].message` contains `could not restore the edit snapshot on Stop`. `task7-playing.json` and `task7-still-playing.json` have `host.state` `playing`. The flag is one-shot, so the second `stop` restores: `task7-restored.json` has `host.state` `ready`. The editor process is still the one this step launched. Do not play or stop an editor the user has open. Quit even when a check fails.

- [ ] **Step 7: Commit.** Suite gate first. The message includes the Step 6 exit codes and artifact paths.

```
Keep Play when Stop fails and abort the save.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 8: CookQueue catches a cook exception and waits with a deadline

**Files:**
- Modify: `Source/Programs/ArcaneEditor/src/Project/CookQueue.cpp:16-34` and `:83-110`
- Do not add a deadline setter. The throw test is the behavior proof. The source pin is the deadline proof.
- Test: `Tests/ArcaneTests/src/CookQueueTest.cpp`

**Interfaces:**
- Consumes: `CookResult` (`CookSession.hpp:110-147`): `failed`, `failures` as `vector<pair<Guid, string>>`.
- Produces: `RunOnePass` wraps `CookProject` in `try`. On `std::exception`, push a `CookResult` with `failed = 1` and `failures.emplace_back(Guid{}, std::string("cook pass threw: ") + ex.what())`. On any other exception, the same with `"cook pass threw"`. Catch inside `RunOnePass` so the exception does not escape the stashed job. `m_running = false` on every exit, including the exception paths and the dirty-loop's normal return. Use a scope object whose destructor clears `m_running` under the mutex so a throw cannot skip it. The dirty loop stays inside the `try` only around `CookProject`; a throw does not immediately start another pass.
- Destructor: lock, if `!m_running` return; else `sleep_for(50ms)` until `!m_running` or 120s have elapsed. On expiry, `ARC_ERROR` that the cook did not finish and `std::abort()`. Do not destroy the queue under a live pass. Do not `yield()`. Today's destructor (`CookQueue.cpp:25-33`) `yield()`s forever while `m_running` is true. A throw from `CookProject` (`:99`) skips `m_running = false` (`:105`), so destroying the queue after a thrown job never returns. The behavior case runs only after this catch and this deadline are both in.

- [ ] **Step 1: Write the source pin only.** Do not construct a throwing `CookQueue` in this step. `NoteChanged` calls `m_submit` under `m_mutex` (`CookQueue.cpp:51-63`); an inline submit deadlocks when the job takes that mutex. The file's `ManualSubmit` (`CookQueueTest.cpp:86`) stashes the job and runs it later from `RunNext`, which is how Step 4 behaves. The RED pin is non-hanging:

```cpp
TEST_CASE("CookQueue's destructor waits with a deadline", "[editor][cook]") {
    const auto text = ReadRepoFile("Source/Programs/ArcaneEditor/src/Project/CookQueue.cpp");
    const auto dtor = text.find("CookQueue::~CookQueue()");
    REQUIRE(dtor != std::string::npos);
    const auto next = text.find("void CookQueue::", dtor + 1);
    REQUIRE(next != std::string::npos);
    const auto body = text.substr(dtor, next - dtor);
    CHECK(body.find("sleep_for") != std::string::npos);
    CHECK(body.find("abort") != std::string::npos);
    CHECK(body.find("yield") == std::string::npos);
}
```

- [ ] **Step 2: RED.** Rebuild (`CookQueue.cpp` is in the test exe, `premake5.lua:1683`). Run `.\ArcaneTests.exe "CookQueue's destructor waits with a deadline"`. Expected FAIL: `yield` is present and `sleep_for` is absent (`CookQueue.cpp:32`). A clean CHECK failure. Do not run a throwing importer. Record the seed.

- [ ] **Step 3: Implement the try and the deadline wait.** The catch and the scope object both run before `RunOnePass` returns, so a thrown cook does not leave `m_running` true.

- [ ] **Step 4: Add the behavior case, then GREEN.** Use `ManualSubmit` (`CookQueueTest.cpp:91`). `SetImporterForTesting` throws `std::runtime_error("boom")`. `NoteChanged` only stashes. `RunNext` runs the job after `NoteChanged` has released `m_mutex`. `Pump`. Require one delivered result, `failed >= 1`, the failure string contains `boom`, and `CookPending()` is false. Destroy the queue only after `CookPending()` is false: if the catch is missing, `RunNext` throws and `~CookQueue` waits forever (today) or 120s and then `abort`s (after Step 3's deadline with no clear of `m_running`). That hang is not a pass. Run this case and `"CookQueue's destructor waits with a deadline"` and the existing cook-queue cases. Expected PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `CookProject(` and `yield()` under `Source/Programs`. A `CookProject` outside `CookQueue` (arccook's main) gets a `try` around the call if it has none, or a `Left as-is:` when it already catches. List `yield()` spins that wait for a flag; fix a sibling that can spin for a whole cook, list the ones that wait on a bounded event.

- [ ] **Step 6: Commit.** Suite gate first.

```
Bound CookQueue exceptions and the destructor wait.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 9: Refuse a mesh index that is not less than vertexCount

**Files:**
- Modify: `Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactReader.cpp:583-596` (the IndexData loop)
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactFormat.cpp:630-643` (the IndexData loop)
- Test: `Tests/ArcaneTests/src/ArtifactReaderTest.cpp` and the pipeline reader test that already builds a mesh artifact

**Interfaces:**
- Consumes: `out.header.vertexCount` (`ArtifactReader.cpp:503` area, stored on the header before the section loop) and `desc.vertexCount` in the pipeline reader.
- Produces: no new type. After reading `idx`, `if (idx >= vertexCount) return std::nullopt;` in both loops. `vertexCount == 0` with any index refuses. `indexCount == 0` does not enter the loop and still succeeds. Do not merge the readers.

- [ ] **Step 1: Write the failing test.** Build a minimal mesh artifact with `vertexCount == 1` and one index `0xFFFFFFFFu`, using the writer the pipeline tests already use (`WriteMeshArtifact`). `ReadClientMeshArtifact` (or the public client read the existing test calls) returns a refusal other than a successful mesh. `ReadMeshArtifact` returns `nullopt`. A second artifact with index `0` and `vertexCount == 1` still loads. An artifact with `indexCount == 0` still loads.

- [ ] **Step 2: RED.** Rebuild and run the new case. Expected FAIL: the `0xFFFFFFFF` artifact is accepted (`ArtifactRefusal::None` / a loaded mesh). Quote that check. Record the seed.

- [ ] **Step 3: Add the comparison in both loops.** Keep the section-range checks that already exist (`ArtifactReader.cpp:644-658`).

- [ ] **Step 4: GREEN.** Both readers refuse the bad index and accept the two controls. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `indices.push_back` and index upload sites under `Source/`. `NriMeshBufferCache.cpp` (the upload the scorecard names near `:217`) stays a consumer: the readers refuse before upload. List it as `Left as-is:` with that sentence. If a third parser builds an index buffer from a file, add the same check.

- [ ] **Step 6: Commit.** Suite gate first.

```
Refuse mesh indices past vertexCount.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 10: Delete every non-current artifact for a guid, and validate the up-to-date file

**Files:**
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactStore.hpp` (declare `RemoveAllExcept`)
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/ArtifactStore.cpp` (after `SweepOrphans`, `:191-212`)
- Modify: `Source/Developer/ArcaneAssetPipeline/src/Arcane/AssetPipeline/CookSession.cpp:367-406`
- Test: `Tests/ArcaneTests/src/AssetPipelineSessionTest.cpp`

**Interfaces:**
- Consumes: `PathFor` (`ArtifactStore.cpp:88-93`), `RebuildIndexFromScan` (`:153-190`, last writer wins), `ReadArtifactPrefix` (`:185`), `ResolveArtifactCandidates` (`Assets.cpp:259-275`, first candidate that validates). Do not change the client walk.
- Produces: `std::size_t ArtifactStore::RemoveAllExcept(const Guid& guid, std::uint64_t keepKey, std::vector<std::filesystem::path>* remaining);`
  - Scan `Artifacts/` the same way `RebuildIndexFromScan` does. For each regular `.arcart` whose prefix `sourceGuid` equals `guid` and whose stem is not `keepKey`, `remove` it. Push paths that are still present afterward into `remaining` (locked file, failed remove). Return the number removed.
- `CookSession` replaces `removeSupersededArtifact` (`:388-395`). On both the up-to-date branch and the freshly written branch, call `RemoveAllExcept(*guid, cookKey, &left)`. If `left` is not empty, this guid is a failure (`result.failed`, a `failures` row `"superseded artifact could not be removed"`, not `upToDate`). The up-to-date branch (`:397-406`) also reads the artifact back with the existing reader for that kind (`ReadTextureArtifact` / `ReadMeshArtifact`). A failed read does not `continue` as up to date; it falls through into the import path. A successful read plus an empty `left` is the only up-to-date outcome.

- [ ] **Step 1: Write the failing test.** Cook a texture, copy its `.arcart` to a second filename under the same guid's hash bucket that still parses as that guid (write a second artifact with the same source guid and a different cook key via the store's writer, or copy the file and patch nothing if the prefix carries the guid — the prefix does, `RebuildIndexFromScan` reads `sourceGuid`). Change a setting so the new key differs and the source hash does not, which is the scorecard's settings-only case. Cook again. Require: only the new key's file remains, and `ResolveArtifactCandidates` on the client (or `ReadClientArtifact` over the directory the test scans) does not return the pre-edit bytes. A second case: plant a file that `exists` but is not a valid artifact at the up-to-date path's sibling, and a truncated file at the current key. The truncated current key must not increment `upToDate`; the cook re-imports.

- [ ] **Step 2: RED.** Rebuild and run the new case. Expected FAIL: the old key's file is still on disk (`removeSupersededArtifact` removes only `previousKeyForGuid`, and a file that was never the indexed key survives). Quote the `exists` check. Record the seed.

- [ ] **Step 3: Implement `RemoveAllExcept` and the validate-or-recook branch.**

- [ ] **Step 4: GREEN.** The new cases and the existing memoization cases PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `removeSuperseded` and `fs::remove(store.PathFor` under `Source/`. One caller remains, and it is `RemoveAllExcept`. `SweepOrphans` deletes by guid for a guid that is gone; leave it, and say so.

- [ ] **Step 6: Commit.** Suite gate first.

```
Delete superseded artifacts and recheck up-to-date bytes.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 11: Shut the job scheduler down before dropping its executor

**Files:**
- Modify: `Source/Runtime/ArcaneCore/src/Arcane/Jobs/JobSystem.cpp:119-126`
- Modify: `Source/Runtime/ArcaneCore/src/Arcane/Jobs/ArcaneWorkScheduler.hpp` (`SetExecutor` is `:39`; confirm before editing)
- Test: `Tests/ArcaneTests/src/JobSystemSubmitTest.cpp`

**Interfaces:**
- Consumes: `enki::TaskScheduler::WaitforAllAndShutdown`, `ArcaneWorkScheduler::SetExecutor(nullptr)` (a null executor runs `ParallelFor` inline as worker 0; the header comment says so).
- Produces: destructor order:
  1. `m_impl->ts.WaitforAllAndShutdown();` while `adapter` and `taskExec` are still alive.
  2. Null the executor on the concrete scheduler. Store `std::shared_ptr<ArcaneWorkScheduler>` in `Impl` (the member is already created as that type at `:116`) and call `SetExecutor(nullptr)` on it. Do not `dynamic_cast`. `WorkScheduler()` still returns `shared_ptr<Mosaic::IWorkScheduler>` (implicit conversion).
  3. `adapter.reset(); taskExec.reset();`
- The post-destruction `ParallelFor` test runs only after this order is in. Today's order access-violates. It is not the RED command.

- [ ] **Step 1: Write the source-order pin** in `JobSystemSubmitTest.cpp`.

```cpp
TEST_CASE("~JobSystem shuts the scheduler down before it resets the adapter", "[jobs]") {
    const auto text = ReadRepoFile("Source/Runtime/ArcaneCore/src/Arcane/Jobs/JobSystem.cpp");
    const auto dtor = text.find("JobSystem::~JobSystem()");
    REQUIRE(dtor != std::string::npos);
    const auto end = text.find("JobSystem::WorkScheduler", dtor);
    const auto body = text.substr(dtor, end - dtor);
    const auto shut = body.find("WaitforAllAndShutdown");
    const auto drop = body.find("adapter.reset");
    REQUIRE(shut != std::string::npos);
    REQUIRE(drop != std::string::npos);
    CHECK(shut < drop);
    CHECK(body.find("SetExecutor(nullptr)") != std::string::npos);
}
```

- [ ] **Step 2: RED.** Rebuild and run that case. Expected FAIL: `shut < drop` is false (`WaitforAllAndShutdown` is at `:125`, `adapter.reset` at `:123`). A clean CHECK failure, not a crash. Record the seed.

- [ ] **Step 3: Reorder the destructor and store the concrete `shared_ptr`.** Keep the existing drain test at `JobSystemSubmitTest.cpp:128` green.

- [ ] **Step 4: Add the kept-scheduler test, then GREEN.** Hold `auto sched = jobs.WorkScheduler();`, destroy `jobs`, call `sched->ParallelFor` over a 4-element range that increments a counter. Require the counter equals 4 and the call returns. Run the order pin and this case and the existing drain test. Expected PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `WaitforAllAndShutdown` and `adapter.reset` under `Source/`. One destructor. List any other pool teardown that resets a worker pointer before joining.

- [ ] **Step 6: Commit.** Suite gate first. This includes `[jobs]`.

```
Drain the job scheduler before dropping its executor.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 12: One JobSystem, owned by the Runtimes on a ProcessContext

**Files:**
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/ProcessContext.hpp` (the private members at `:50-56`)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/ProcessContext.cpp`
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.cpp:198-240` (`Impl::jobs` and the ctor initializer) and `:459-460` (`TaskExecutor()` and `Jobs()`)
- Modify: `Source/Runtime/ArcaneEngine/src/Arcane/Base/Runtime.hpp:147-156` (the `Jobs()` comment)
- Test: `Tests/ArcaneTests/src/RuntimeTest.cpp` (beside the two-Runtime case at `:462`)

**Interfaces:**
- Consumes: `ApplyEngineRungAndResolveWorkers()` (`Runtime.cpp:131-136`). It is in the anonymous namespace of `Runtime.cpp` and `PublishImmediate` asserts the main thread. `ProcessContext.cpp` cannot call it. `JobSystem` from Task 11. `ProcessContext::Create` refuses a second instance.
- Produces:
  - `std::shared_ptr<JobSystem> ProcessContext::AcquireJobSystem(std::uint32_t workerCount);` Holds `m_jobsMutex` across the whole check-and-mint. `m_jobs` is `std::weak_ptr<JobSystem>`. If `m_jobs.lock()` succeeds, return that pointer and ignore `workerCount`. If it is expired, `auto made = std::make_shared<JobSystem>(workerCount);`, store the weak, return `made`, still under the same lock. Two Runtime constructors that overlap cannot each observe an expired weak and each mint.
  - `Runtime::Impl` holds `std::shared_ptr<JobSystem> jobs;` instead of `JobSystem jobs;`. The ctor initializer is `jobs(proc.AcquireJobSystem(ApplyEngineRungAndResolveWorkers()))`. The count is resolved in `Runtime.cpp` before the call. `sched` is still `jobs->WorkScheduler()`. `~Runtime` drops the `shared_ptr` and does not call `WaitforAllAndShutdown` itself. The last Runtime's drop runs `~JobSystem`. `~ProcessContext` does not keep a strong ref; its weak expires with the last Runtime.
  - `Runtime::TaskExecutor()` (`Runtime.cpp:459`) returns `m_impl->jobs->TaskExecutor()`. `Runtime::Jobs()` (`:460`) returns `*m_impl->jobs`. Both still match the signatures in `Runtime.hpp:148` and `:156`.
  - Direct `JobSystem` locals in tests (`JobSystemSubmitTest.cpp`, `JobSchedulerTest.cpp`, `TaskExecutorTest.cpp`, `SettingsBindingsTest.cpp`, `CVarSnapshotTest.cpp`) stay. The one-per-process rule is for Runtimes.
- `Jobs()` still returns `JobSystem&`. The comment at `Runtime.hpp:149-156` says the reference is the process pool, alive as long as any Runtime on this `ProcessContext` is alive. It no longer says the `JobSystem` is a by-value member of one Runtime.

- [ ] **Step 1: Write the failing gate test.**

```cpp
TEST_CASE("two Runtimes share one JobSystem and the survivor can Submit", "[runtime][jobs]") {
    auto& proc = *Arcane::Test::Process();
    Arcane::Runtime a(proc, Arcane::NetMode::Standalone);
    Arcane::Runtime b(proc, Arcane::NetMode::Standalone);
    CHECK(&a.Jobs() == &b.Jobs());
    std::atomic<int> n{0};
    a.Jobs().Submit([&] { n.fetch_add(1); });
    // Destroy a by leaving an inner scope. b.Jobs().Submit still runs.
    // Require n == 2 after both submits have been observed (the destructor
    // drains on the last drop only; wait by destroying a copy of the
    // shared work: call b.Jobs() after a is gone and Submit a second task,
    // then destroy b and REQUIRE n == 2).
}
```

  Structure the scopes so `a`'s destructor runs while `b` is alive, then the second `Submit` goes through `b`, then `b` is destroyed and the drain of Task 11 runs the tasks. `REQUIRE(n == 2)`.

- [ ] **Step 2: RED.** Full `build.ps1` (Runtime and ProcessContext are engine modules). Run the case. Expected FAIL: `&a.Jobs() == &b.Jobs()` is false (each `Impl` constructs its own `JobSystem` at `Runtime.cpp:239`). A clean CHECK failure. Record the seed.

- [ ] **Step 3: Implement the weak slot.** Do not put a `shared_ptr` member that `ProcessContext` keeps for its own lifetime. `AcquireJobSystem` is the only mint, and it does not call `ApplyEngineRungAndResolveWorkers`. Update `Runtime.cpp:459-460` in the same edit as the member type; leaving `m_impl->jobs.TaskExecutor()` or `return m_impl->jobs` does not compile once `jobs` is a `shared_ptr`. The mutex stays held from the `lock()` through the `make_shared` and the weak store. Hosts already destroy every `Runtime` before `~ProcessContext`; say that in the `ProcessContext` comment.

- [ ] **Step 4: GREEN.** The gate case PASS. Then the suite gate, which runs `SettingsBindingsTest.cpp` (`:249-308`) in random order. Expected: those worker-count checks still pass, because no sibling Runtime is alive inside them and the weak is expired, so the new pool reads the cvar they just set. If a worker-count check fails, a strong ref leaked. Do not "fix" it by deleting the check. Record both seeds.

- [ ] **Step 5: Sibling sweep.** Grep `JobSystem` constructions under `Source/`. Production constructions are `AcquireJobSystem` only. Test-local constructions are `Left as-is:` with the file names.

- [ ] **Step 6: Commit.** Suite gate first. This is the readiness gate: the commit message says two Runtimes compared equal and the survivor's `Submit` ran.

```
Share one JobSystem across the Runtimes on a ProcessContext.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 13: CI seeds are arithmetic and stay inside uint32

**Files:**
- Modify: `.github/workflows/ci.yml:163` and `:305` (`SEED_B`)
- Modify: `scripts/ci/ArcaneCi.psm1:49-59` (`Get-SeedList`)
- Modify: `scripts/ci/tests/test-arcane-ci.ps1:45-48` (the concatenation pins)

**Interfaces:**
- Produces: `SEED_B = (SEED_A + 50021) mod 4294967296`. GitHub Actions expressions have no `+` or `%` (`docs.github.com` expression operator list), so the workflow must not compute this in `${{ }}`. Delete the job-env `SEED_A` / `SEED_B` lines at both sites (`ci.yml:162-163` and `:304-305`). Insert this step immediately after `actions/checkout` in each job, before any step that reads the variables (`:194` and `:425`). `GITHUB_ENV` applies to subsequent steps, including the Windows `pwsh` test step's `$env:SEED_A`.

```yaml
- name: Derive the uint32 CI seeds
  shell: bash
  run: |
    set -euo pipefail
    seed_a="${{ github.run_number }}"
    case "$seed_a" in
      ''|*[!0-9]*) echo "run_number is not an integer" >&2; exit 1 ;;
    esac
    if [ "$seed_a" -gt 4294967295 ]; then
      echo "run_number does not fit uint32" >&2
      exit 1
    fi
    # bash $(( )) is signed 64-bit. This sum fits.
    seed_b=$(( (seed_a + 50021) % 4294967296 ))
    echo "SEED_A=${seed_a}" >> "$GITHUB_ENV"
    echo "SEED_B=${seed_b}" >> "$GITHUB_ENV"
```

  `${{ github.run_number }}` is a string pasted into the script. It is not an arithmetic expression. A step output (`echo "seed_b=$seed_b" >> "$GITHUB_OUTPUT"`) is the same number if a later step prefers `env:`; this plan uses `$GITHUB_ENV` because the test steps already expand `$SEED_A` and `$env:SEED_A`. `Get-SeedList` computes `[uint64]$Seed + 50021`, then subtracts `4294967296` while the sum is greater than `4294967295`, and appends that decimal. It throws when `$Seed` itself does not fit in uint32. The comment that says concatenation and "keep -Seed below 42950" is rewritten to describe the add.

- [ ] **Step 1: Update the pins and RED.** `scripts/ci/tests/test-arcane-ci.ps1` currently expects `'123,12350021'` (`:45`), `'42949,4294950021'` (`:46`), and a throw on `'99999'` with `-Seeds 2` (`:48`). Replace those three:

```powershell
Check 'seeds: the CI pair' ((((Get-SeedList -Seed '123' -Seeds 2) -join ',')) -eq '123,50144')
Check 'seeds: 42949 pair stays in uint32' ((((Get-SeedList -Seed '42949' -Seeds 2) -join ',')) -eq '42949,92970')
Check 'seeds: wrap stays in uint32' ((((Get-SeedList -Seed '4294967295' -Seeds 2) -join ',')) -eq '4294967295,50020')
Check 'seeds: a seed past uint32 is refused' (Throws { Get-SeedList -Seed '4294967296' })
```

  `123 + 50021 = 50144`. `42949 + 50021 = 92970`. `(4294967295 + 50021) mod 4294967296 = 50020`. Delete the `'99999'` throw: `99999 + 50021` fits, so that pin would stay red for the wrong reason. Run `powershell -NoProfile -File scripts\ci\tests\test-arcane-ci.ps1` before editing `Get-SeedList`. Expected FAIL: `seeds: the CI pair` prints the got value `123,12350021`. Quote it.

- [ ] **Step 2: Implement `Get-SeedList` and both YAML sites.** The shell step above is the whole workflow change. Do not leave a `${{ }}` expression that adds or mods.

- [ ] **Step 3: GREEN.** `powershell -NoProfile -File scripts\ci\tests\test-arcane-ci.ps1` exits 0. Grep `50021` in `ci.yml`. Both hits are `seed_a + 50021` inside the shell step. Neither site still has `${{ github.run_number }}50021` or a `${{ (` arithmetic expression.

- [ ] **Step 4: Sibling sweep.** Grep `50021` and `--rng-seed` under `scripts/` and `.github/`. One formula. List any other seed concatenation.

- [ ] **Step 5: Commit.** No engine build.

```
Derive the second CI seed by addition.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 14: A missing baseline fails, and a seed with no summary fails

**Files:**
- Modify: `scripts/check-baselines.ps1:123-125` (the `continue` on a missing entry)
- Create: `scripts/check-baselines.py`
- Create: `scripts/baseline-exemptions.json`
- Modify: `scripts/automation-baselines.json` (add the measured unfiltered rows only)
- Modify: `scripts/run-tests.sh` (emit Catch2 JSON and run the python checker)
- Modify: `scripts/ci/ArcaneCi.psm1:325-347` (no summary sets `$passed = $false`)
- Modify: `Jenkinsfile:81-99` (the comment that a missing unfiltered baseline exits 0)

**Interfaces:**
- Produces:
  - `check-baselines.ps1`: a missing `(name, configuration, invocation)` entry exits 1 unless `scripts/baseline-exemptions.json` contains that pair with a non-empty `reason`. The exemption schema is `{ "exemptions": [ { "configuration": "Dist", "invocation": "~[gpu] hosted-ci", "reason": "..." } ] }`.
  - The one exemption this round writes: configuration `Dist`, invocation `~[gpu] hosted-ci`, reason `The desk must not set CI or ARCANE_BUILD_MACHINE. Hosted Dist skips CrashPathTest reporter cases only when CI is set, so this lane has no measured count until a hosted Dist JSON artifact is copied in.`
  - Unfiltered Debug and unfiltered Release get real rows. Do not invent the integers. Measure them one at a time, desk-cap preamble, `CI` and `ARCANE_BUILD_MACHINE` unset, from the exe directory. Jenkins' unfiltered lane is a bare `ArcaneTests.exe` (no `~[gpu]`), which is what `-Invocation unfiltered` means (`check-baselines.ps1:28-29`). Write the JSON to `test-results\unfiltered-Debug.json` and `test-results\unfiltered-Release.json`:

```powershell
.\ArcaneTests.exe -r console -r "json::out=D:\dev\starworks\Arcane-fixes\test-results\unfiltered-Debug.json"
```

  Copy `test-run.totals` assertions (`passed + failed`) and test-cases (`passed + failed`) into `automation-baselines.json` under invocation `unfiltered`. Record both `Randomness seeded to:` lines in the commit body. Release is a second `build.ps1 Release -Jobs 4` under the same preamble, then the same exe command with `unfiltered-Release.json` from the Release exe dir. Do not measure Dist.
  - `~[gpu] hosted-ci` Debug and Release already have rows. Leave them.
  - `scripts/check-baselines.py` reads the same JSON and the same exemption file and exits 1 on a missing unexempted entry or a regressed count. `run-tests.sh` adds Catch2's JSON reporter next to junit and runs `python3 scripts/check-baselines.py` (the Linux image has no `pwsh`; it has `python3`). `run-tests.ps1` keeps calling the ps1, which now fails closed.
  - `New-LegResult` (`ArcaneCi.psm1:337-340`): in the `else` of `if ($t.Found)`, set `$passed = $false`. `$desc` stays `no Catch2 summary in the log`. The later "tests did not run" branch still applies when `$tests.Count` is 0.

- [ ] **Step 1: RED the no-summary hole.** The hole is `New-LegResult` in `scripts/ci/ArcaneCi.psm1:337-340`: a seed whose status is 0 and whose log has no Catch2 summary leaves `$passed` true. `scripts/ci/tests/test-arcane-ci.ps1` already calls `New-LegResult` (`:155`). Add this check beside it, and write the empty log first:

```powershell
Put (Join-Path $leg 'tests-seed8.log') "build output only, no catch2 summary`n"
$nosum = New-LegResult -Run $run -Status ([ordered]@{ build = 0; 'tests-8' = 0 }) -OutDir $leg -Seeds @('8') -Image 'build-x'
Check 'result: a seed with no Catch2 summary fails the leg' (-not $nosum.Passed)
```

  Run `powershell -NoProfile -File scripts\ci\tests\test-arcane-ci.ps1` before editing `New-LegResult`. Expected FAIL: `result: a seed with no Catch2 summary fails the leg`. Quote that line. (`$passed` was set true by `build = 0` and the else branch at `:337` does not clear it.)

- [ ] **Step 2: RED the missing baseline.** `check-baselines.ps1` reads Catch2 JSON (`-ReportPath`, `-Configuration`, `-Invocation`), not junit. Write this file to `$env:TEMP\arcane-baseline-probe.json` and run the checker before editing it:

```json
{"test-run":{"totals":{"assertions":{"passed":1,"failed":0},"test-cases":{"passed":1,"failed":0}}}}
```

```powershell
powershell -NoProfile -File D:\dev\starworks\Arcane-fixes\scripts\check-baselines.ps1 -ReportPath $env:TEMP\arcane-baseline-probe.json -Configuration Debug -Invocation unfiltered
```

  Expected: exit 0 and the host line contains `NO BASELINE -- not checked` (`check-baselines.ps1:123-125`). That exit 0 is the failure this task removes.

- [ ] **Step 3: Implement the fail-closed ps1, the python twin, the exemption, and the `ArcaneCi.psm1` assignment.** Then measure unfiltered Debug and Release under the desk cap (the suite is the heavy process; the preamble's `ArcaneTests` check applies). Write the two rows with the numbers the exe printed. Update the Jenkinsfile comment so it no longer says a missing entry exits 0.

- [ ] **Step 4: GREEN.** `powershell -NoProfile -File scripts\ci\tests\test-arcane-ci.ps1` exits 0, including `result: a seed with no Catch2 summary fails the leg`. `check-baselines.ps1 -ReportPath D:\dev\starworks\Arcane-fixes\test-results\unfiltered-Debug.json -Configuration Debug -Invocation unfiltered` exits 0. The same command with `-Invocation "no-such-lane"` exits 1. `python3 scripts/check-baselines.py` takes `-ReportPath`, `-Configuration`, and `-Invocation` and agrees. `-Configuration Dist -Invocation "~[gpu] hosted-ci" -ReportPath` the probe JSON exits 0 because of the exemption and prints the reason.

- [ ] **Step 5: Sibling sweep.** Grep `NO BASELINE` and `not checked`. One message remains, and it is followed by exit 1 unless exempted. `run-tests.sh` calls the checker. List any other suite entry that writes junit and does not check.

- [ ] **Step 6: Commit.**

```
Fail closed when a test lane has no baseline or no summary.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 15: Hosted CI replays fuzz regressions and namespace compile-fail

**Files:**
- Modify: `.github/workflows/ci.yml` (a step on the Linux gcc Debug leg, after the tree is configured and before the test loop that starts near `:180`)

**Interfaces:**
- Consumes: `scripts/namespace-compile-fail.sh`, `scripts/run-fuzz.sh regress`. Do not add a fuzzer. Do not change their arguments.
- Produces: one workflow step, `bash scripts/namespace-compile-fail.sh` and `bash scripts/run-fuzz.sh regress`, on `linux-gcc-14` Debug only (that leg already has a compiler and is the `[gpu]` lane's host; the replay is CPU). `if: matrix.leg == 'linux-gcc-14' && matrix.config == 'Debug'`. A non-zero exit fails the leg.

- [ ] **Step 1: Confirm both scripts exist and are not referenced.** Grep `namespace-compile-fail` and `run-fuzz` in `.github/`. Expected today: no hits in `ci.yml` (the scorecard's M28). Quote the grep.

- [ ] **Step 2: Add the step.** Read each script's usage header and pass only the arguments it documents for replay / compile-fail. `run-fuzz.sh regress` is the replay (no campaign).

- [ ] **Step 3: Run both scripts locally** from the repo root, one at a time, after the desk-cap preamble if they compile. Expected: exit 0. If `namespace-compile-fail.sh` needs a compiler flag this desk does not have, run the `.ps1` twin the header names and say so in the commit body. A failure is a real regression. Stop and report. Do not weaken the script.

- [ ] **Step 4: Sibling sweep.** Grep `scripts/*.sh` for a regression entry point the workflow does not call. List any you did not add, with why (a campaign fuzzer stays out).

- [ ] **Step 5: Commit.**

```
Replay fuzz regressions and namespace compile-fail in hosted CI.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 16: A pull request builds the CI image and does not push it

**Files:**
- Modify: `.github/workflows/ci.yml:87-125` (the `ci-image` job env) and `:127-139` (the Linux legs' `container:`)
- Modify: `scripts/ci/ci-image.sh:25-40`

**Interfaces:**
- The Linux legs pull `ghcr.io/starworksdev/arcane-ci@${{ needs.ci-image.outputs.digest }}` (`ci.yml:138-139`). A digest has to be a registry manifest. A local `--load` image has no registry digest, so a later job cannot pull it.
- Produces:
  - Env `PUBLISH` is `'true'` only when `github.event_name != 'pull_request' && github.ref == 'refs/heads/main'`. `workflow_dispatch` on main publishes. A pull request never publishes. Forks stay on today's failure (`SAME_REPO` is not true for a fork).
  - `ci-image.sh`: if `imagetools inspect` succeeds, reuse the tag and do not push (unchanged). If it fails and `SAME_REPO` is true and `PUBLISH` is true, build, verify, push (today's push path). If it fails and `SAME_REPO` is true and `PUBLISH` is not true, build, verify, do not push, write `digest=` empty and `verified=local` to `GITHUB_OUTPUT`, exit 0. If `SAME_REPO` is not true, the existing fork error, exit 1.
  - Linux legs gain `if: needs.ci-image.outputs.digest != ''`. A pull request whose Dockerfile hash is already published runs the legs on that digest. A pull request that introduces a new tag verifies the image in `ci-image` and skips the Linux legs until main publishes. That skip is the cost of not pushing an unreviewed Dockerfile. The job summary step in `ci-image` prints that sentence when `verified=local`.

- [ ] **Step 1: Write the policy test, then RED.** Create `scripts/ci/test-ci-image-policy.sh`. It extracts `publish_decision` from `scripts/ci/ci-image.sh` with `sed -n '/^publish_decision()/,/^}/p'`, exits 1 with the text `publish_decision missing` when that extract is empty, otherwise `eval`s it and checks:

```bash
INSPECT_OK=true SAME_REPO=true PUBLISH=true
test "$(publish_decision)" = reuse
INSPECT_OK=false SAME_REPO=true PUBLISH=true
test "$(publish_decision)" = push
INSPECT_OK=false SAME_REPO=true PUBLISH=false
test "$(publish_decision)" = local-verify
INSPECT_OK=false SAME_REPO=false PUBLISH=true
test "$(publish_decision)" = refuse
```

  The function is not in `ci-image.sh` yet. Run `bash scripts/ci/test-ci-image-policy.sh` from Git Bash or WSL. Expected FAIL: stdout contains `publish_decision missing` and the exit code is 1. Do not run `docker`. Quote `ci-image.sh:36` (`docker push`) as the line Step 2 guards.

- [ ] **Step 2: Add `publish_decision` to `ci-image.sh` and set `PUBLISH` in the workflow.** The function:

```bash
# reuse | push | local-verify | refuse
publish_decision() {
    if [ "${INSPECT_OK:-false}" = true ]; then echo reuse; return 0; fi
    if [ "${SAME_REPO:-false}" != true ]; then echo refuse; return 0; fi
    if [ "${PUBLISH:-false}" = true ]; then echo push; return 0; fi
    echo local-verify
}
```

  `INSPECT_OK=true` when `docker buildx imagetools inspect` succeeds. `docker push` runs only when the function prints `push`. `local-verify` builds, runs `verify-image.sh`, writes `digest=` empty and `verified=local`, and does not push. `refuse` is today's fork error, exit 1. `reuse` is today's published-tag path. Workflow env: `PUBLISH: ${{ github.event_name != 'pull_request' && github.ref == 'refs/heads/main' }}`.

- [ ] **Step 3: GREEN.** `bash scripts/ci/test-ci-image-policy.sh` exits 0. GitHub-expression review: `PUBLISH` on a `pull_request` is the string `false`. Do not run a live push from this desk.

- [ ] **Step 4: Sibling sweep.** Grep `docker push` under `scripts/` and `.github/`. One push, guarded by `PUBLISH`. List any other.

- [ ] **Step 5: Commit.**

```
Verify CI images on pull requests without pushing them.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 17: Server startup fails when a requested remote fails, and a failed report is a non-zero exit

**Files:**
- Modify: `Source/Programs/ArcaneServer/src/ServerApp.cpp:54-59` (`Finish`) and `:209-210` (Start failure)
- Test: `Tests/ArcaneTests/src/ServerAppStartupTest.cpp` (new). No test constructs `ServerApp` (the hits under `Tests/` are comments). The pin below is the test. Do not add a process harness.

**Interfaces:**
- `Finish`: if `WriteTo` fails and `exitCode == 0`, return `1`. If `exitCode != 0`, return `exitCode` unchanged. Log stays.
- Start failure (`:209-210`): replace `m_remote.reset();` with `return Finish(rep, "remote-failed", 2);`. Do not carry on. The refuse path at `:180-187` already returns 2. Idle-exit at `:196` stays.

- [ ] **Step 1: Write the source pin.**

```cpp
TEST_CASE("a requested remote Start failure returns Finish, and a failed report cannot exit 0", "[server]") {
    const auto text = ReadRepoFile("Source/Programs/ArcaneServer/src/ServerApp.cpp");
    const auto start = text.find("if (!m_remote->Start())");
    REQUIRE(start != std::string::npos);
    const auto window = text.substr(start, 180);
    CHECK(window.find("return Finish") != std::string::npos);
    CHECK(window.find("m_remote.reset()") == std::string::npos);
    const auto finish = text.find("ServerApp::Finish");
    const auto finishBody = text.substr(finish, 500);
    CHECK(finishBody.find("return 1") != std::string::npos);
}
```

- [ ] **Step 2: RED.** Rebuild ArcaneTests (test-only: `/t:ArcaneTests` after the preamble). Run the case. Expected FAIL: `window.find("return Finish")` is npos, and `m_remote.reset()` is present. Record the seed.

- [ ] **Step 3: Edit the two sites.** `Finish` returns 1 when `WriteTo` fails and the passed code is 0, and returns the passed code when it is already non-zero. The Start-failure branch returns `Finish(rep, "remote-failed", 2)`.

- [ ] **Step 4: GREEN.** The pin PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `->Start()` on a remote session under `Source/Programs`. `RuntimeApp.cpp` already exits 2 when `--remote` was requested and the surface did not come up for an open-ended headless run (`:860-873` area; re-read the listener-failure branch and quote it). If the runtime's Start-failure path still continues on an open-ended headless run, fix it the same way. A windowed editor that logs and continues is `Left as-is:` only when the comment says the editor must not exit; quote the comment.

- [ ] **Step 6: Servitor.** Drive the failed Start. A ready server never takes `ServerApp.cpp:209-210`. An occupied port does not take it either: the listener binds ephemeral port 0 (`Listener.cpp:1076`, `CreateListenSocket(0, "127.0.0.1")`) with `SO_REUSEADDR` (`TcpSocket.hpp:200`). The induction that returns false from `Start()` is an unwritable `--remote-descriptor`, the same shape as `HostSessionTest.cpp:87-110`: the parent of the descriptor path is a regular file, so `WriteDescriptorAtomic`'s `create_directories` fails (`Descriptor.cpp:187-197`). The log line is `remote: could not write the descriptor` (`HostSession.cpp:275`) or `remote: the loopback listener did not start` (`:252`). `launch` adds `--remote` (`Launch.cpp:427-428`) and accepts `--remote-descriptor` as its own flag (`Verbs.cpp:137`). It takes no `--client` until Task 21. Do not `wait settled`: the host is supposed to die before ready. `--json` on this launch is a successful `ParseArgs` (the format is finalized); it is not the Task 20 hole.

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$dir = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1'
$blocker = Join-Path $dir 'task17-blocker'
if (Test-Path $blocker) { Remove-Item -Force $blocker }
New-Item -ItemType File -Path $blocker -Force | Out-Null
$desc = Join-Path $blocker 'server.json'
$report = Join-Path $dir 'task17-report.json'
$server = Join-Path $dir 'task17-server-launch.json'
$runtime = Join-Path $dir 'task17-runtime-launch.json'
$engineLog = 'D:\dev\starworks\Arcane-fixes\ReferenceProject\Saved\Logs\ArcaneServer.log'
$before = 0
if (Test-Path $engineLog) { $before = (Get-Item $engineLog).Length }
& $s --project ReferenceProject --json launch server --headless --remote-descriptor $desc -- --report $report | Out-File -Encoding utf8 $server
# record $LASTEXITCODE
$rtLog = 'D:\dev\starworks\Arcane-fixes\ReferenceProject\Saved\Logs\ArcaneRuntime.log'
$rtBefore = 0
if (Test-Path $rtLog) { $rtBefore = (Get-Item $rtLog).Length }
& $s --project ReferenceProject --json launch runtime --headless --remote-descriptor $desc -- --fixed-time 0 | Out-File -Encoding utf8 $runtime
# record $LASTEXITCODE
```

  `PlanLaunch` puts the servitor launch log under the descriptor's parent (`Launch.cpp:422`), which is the blocker file, so `errors[0].detail.log` is a path that was not created. That missing file is expected. The engine log is the one `HostLogFile` names (`Client.cpp:724-738`): `<project>/Saved/Logs/<app>.log`, because diagnostics live at `<project>/Saved/Diagnostics` (`Paths.hpp:33`). Record both launch exit codes, both json paths, `$report`, and the two engine logs. Expected for the server, after this task's `return Finish(rep, "remote-failed", 2)`: exit 7, `$server` has `errors[0].code` `host.died` and `errors[0].detail.exitCode` `2` (`Launch.cpp:541-551`), `$report` parses and its `exitReason` is `remote-failed` (`ServerReport.cpp:40`), and the bytes of `ArcaneServer.log` after `$before` contain `could not write the descriptor` or `the loopback listener did not start`. The process has exited; do not `quit` it. Exit 8 means it is still in the tick loop (`m_remote.reset()` then the loop at `:228`); `quit --pid` the `detail.pid` and the step has failed.

  The runtime launch is the sibling that already exits. `RuntimeApp.cpp:970-979` sets `m_remoteArmExit = 2` when an open-ended headless `--remote` did not start. Expected: exit 7, `detail.exitCode` `2`, and the bytes of `ArcaneRuntime.log` after `$rtBefore` contain that start failure or `exiting 2`. Same rule for exit 8: quit the pid, the step has failed.

  Do not point this descriptor at an editor. A headless editor with a null session already exits 2 on the next loop pass (`EditorAppFrame.cpp:484-489`) after Start failure resets it (`:2110-2114`); that guard is not this task's edit. The comment at `:1958-1959` is a later re-arm (ClientOnly play, a project switch), which logs and does not end the editor. Step 5 quotes it and leaves it. Do not quit a server or runtime this step did not launch.

- [ ] **Step 7: Commit.** Suite gate first. The message includes the Step 6 exit codes and the log path.

```
Fail the server when --remote or --report did not succeed.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 18: A held frame presents derived transforms and bounds

**Files:**
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.hpp` (declare `PresentDerivedState`)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp:229-237`
- Test: `Tests/ArcaneTests/src/RuntimeHoldPresentTest.cpp` (new)

**Interfaces:**
- Consumes: `TransformPropagationSystem::operator()` (`TransformSystems.hpp:200`) and `BoundsSystem::operator()` (`BoundsSystem.hpp`). `EditModeSchedule.cpp:14-23` runs that pair outside Play. `io.runtime->Registry()` is the registry (`RuntimeFrame.cpp:415`).
- Produces: `void PresentDerivedState(Astra::Registry& reg);` in the `RuntimeFrame` TU (anonymous namespace is fine if the test does not link the TU). Body:

```cpp
void PresentDerivedState(Astra::Registry& reg) {
    Arcane::TransformPropagationSystem{}.operator()(reg);
    Arcane::BoundsSystem{}.operator()(reg);
}
```

  Call it in the Hold branch after the clock update (`:236`) and before `return` (`:237`). Do not call `RunLoop::Advance`. Do not increment `io.frameCount`. Do not step physics. Call it on every Hold, including `settleHold` (the clock stays frozen; derived state still has to exist). The system's own tick advance inside `operator()` stays; that is how `Changed<>` publishes. It is not `host.frame`.

- [ ] **Step 1: Write the source pin and a behavior test that calls the same two operators.** The behavior test builds a registry with a parent and a child `Transform` (child local translation `(1, 0, 0)`, parent `(2, 0, 0)`), runs the two operators, and checks the child's `WorldTransform` translation X is 3 and a sprite on the child has a non-empty `WorldBounds`. That behavior is already true of the systems; it documents the pair. The RED is the pin:

```cpp
TEST_CASE("Hold presents world transform and bounds without advancing frameCount", "[runtime][hold]") {
    const auto text = ReadRepoFile("Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp");
    const auto hold = text.find("if (pace == SimPace::Hold)");
    REQUIRE(hold != std::string::npos);
    const auto ret = text.find("return;", hold);
    const auto body = text.substr(hold, ret - hold);
    CHECK(body.find("PresentDerivedState") != std::string::npos);
    CHECK(body.find("RunLoop::Advance") == std::string::npos);
    CHECK(body.find("frameCount") == std::string::npos);
}
```

- [ ] **Step 2: RED.** Rebuild ArcaneTests. Run `"[runtime][hold]"`. Expected FAIL: `PresentDerivedState` is absent in the Hold window. Record the seed.

- [ ] **Step 3: Add the function and the call.** Include `BoundsSystem.hpp` and `TransformSystems.hpp` from `RuntimeFrame.cpp`.

- [ ] **Step 4: GREEN.** The pin PASS. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `SimPace::Hold` under `Source/`. One Hold branch. The editor's edit-mode schedule already runs the pair (`EditModeSchedule.cpp:14-23`). Leave it.

- [ ] **Step 6: Do not commit yet if Task 19 is next in the same sitting.** Commit at the end of Task 19 so the pixel witness and this pin share a revision. If you commit here, the message is:

```
Present derived transforms and bounds while the sim is held.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 19: A Servitor screenshot at frame 0 shows world pixels

**Files:**
- No new engine file if Task 18 landed. Evidence files under `test-results\review-round-1\` (gitignored is fine; the commit message carries the paths and the exit codes).

**Interfaces:**
- Consumes: the Hold change from Task 18. Servitor from the Global Constraints. `describe screenshot` is the authority for the headless source name; do not guess a flag the describe text does not list.

- [ ] **Step 1: Build** with the desk-cap preamble so `ArcaneRuntime.exe` and `servitor.exe` are this tree's Debug binaries.

- [ ] **Step 2: Launch and shoot.**

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$out = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1\hold-frame0.png'
& $s --project ReferenceProject launch runtime --headless -- --fixed-time 0
# record $LASTEXITCODE
& $s --project ReferenceProject --client grok-review-r1 --host runtime wait settled
& $s --project ReferenceProject --client grok-review-r1 --host runtime describe screenshot
# use the source that reads the offscreen canvas
& $s --project ReferenceProject --client grok-review-r1 --host runtime screenshot window --out $out
& $s --project ReferenceProject --client grok-review-r1 --host runtime stats frame
& $s --project ReferenceProject --client grok-review-r1 --host runtime quit
```

  `launch` has no `--client` until Task 21. The verb is `stats frame` (`Verbs.cpp:134`). Record every exit code. `data.frame` (`StatsCommands.cpp:64`) is 0. The PNG is not a flat clear: decode it (a few lines of Python, or the existing image helper under `Tests/`) and require at least one pixel whose RGB differs from the canvas clear `{0.02, 0.02, 0.04}` by more than the usual 8-bit quantization, outside the HUD band if the HUD is drawn. A uniform clear is a failed check (exit-code 8 if you express it as a compare; otherwise a failed assertion in the commit's evidence). Quit even when the check fails.

- [ ] **Step 3: Golden gate.** `scripts\golden-gate.ps1`. Required: 14 lanes, each `diffCount` 0. No `--bless`. A diff stops the round.

- [ ] **Step 4: Commit** Tasks 18 and 19 together if 18 was not committed. Message includes the exit codes, the PNG path, `stats` frame 0, and `14/14 diffCount 0`.

```
Show world pixels on a held frame 0.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 20: json and ndjson write the envelope on stdout and leave stderr empty

**Files:**
- Modify: `Source/Programs/Servitor/src/Servitor/Client.cpp:469-472` (`PrintUsage`) and `:557-561` (`--verbose` uses `ARC_INFO`)
- Modify: `Source/Programs/Servitor/src/Servitor/Format.cpp:106-122` and `Format.hpp:22-29` (the comment that says diagnostics go to stderr)
- Modify: `Source/Programs/Servitor/src/Servitor/Commands.cpp` every `Write(stderr, printed.stderrText)` that follows a json render (the list verb at `:383` is one; grep the rest)
- Test: `Tests/ArcaneTests/src/Remote/CommandsCliTest.cpp`

**Interfaces:**
- Human format is unchanged: usage text and diagnostics on stderr.
- json and ndjson: `PrintUsage` writes one envelope to stdout and does not write stderr. The envelope is `ok: false`, `errors[0].code` is `arg.invalid`, `errors[0].message` is the problem string, and the rendered client block's `exitCode` is 2. Return 2.
- The `!ParseArgs` return at `Client.cpp:618-619` passes a format scanned from `argv` before `ParseArgs` runs, stopping at `--`. `--json` selects Json. `--format json|ndjson|human` selects that format unless `--json` is also present. No such flag selects Human, the `Client.hpp:57` default, for this return only.
- `--verbose` on json and ndjson appends the redacted request line as a warning on that same envelope (`code` `servitor.verbose`), still on stdout. It does not call `ARC_INFO`. Human `--verbose` may keep `ARC_INFO`. There is no `--debug-stderr` switch.
- `Render` for json and ndjson leaves `stderrText` empty. Errors live in the envelope, which is `stdoutText`.

- [ ] **Step 1: Write the failing test.** `status` never posts a call, so `--json --verbose status` does not reach `ARC_INFO` at `Client.cpp:557`. The verbose half goes through `Servitor::Call` and the fake exchange already in this file (`CaptureExchange`, `CommandsCliTest.cpp:168`), which records the body and returns `MakeOk`. Capture both process streams around that call. `ARC_INFO` writes the engine logger's stderr sink (`Log.cpp` builds `stderr_color_mt("Arcane")`), not `Printed.stderrText`. Dup stdout and stderr the way `HostConfigTest.cpp:920` dups stderr (`StderrCapture`), restore before reading, and `Emit` the `Printed` `Call` returns so the envelope lands on the captured stdout.

```cpp
TEST_CASE("json usage and a verbose posted call leave stderr empty", "[remote][cli]") {
    // Early ParseArgs failures, before out.format is assigned (Client.cpp:227-234).
    // GlobalOptions::format defaults to Human (Client.hpp:57), so PrintUsage at
    // Client.cpp:618-619 writes stderr unless the format was scanned first.
    //
    // Run({"servitor", "--json", "--host"}) -> exit 2.
    //   --host needs a value (Client.cpp:181-182) and returns before line 227.
    // Run({"servitor", "--format", "json", "--timeout"}) -> exit 2.
    //   --timeout needs a value (Client.cpp:174-176) and returns before line 227.
    // Run({"servitor", "--json", "--format", "not-a-format"}) -> exit 2.
    //   ParseFormat fails (Client.cpp:160-162) after --json was seen and before
    //   out.format is assigned. --json wins, matching line 227.
    // Each: stdout is one json envelope, ok == false, errors[0].code == "arg.invalid",
    // the rendered client exitCode is 2, stderr is empty.
    //
    // Verb-match failure, after ParseArgs has set out.format (not the hole above):
    // Run({"servitor", "--json", "--not-a-flag"}) -> exit 2, same envelope,
    // stderr empty. An unknown token goes to rest (Client.cpp:221-222), ParseArgs
    // returns true, and MatchVerb fails at Client.cpp:629-631.
    //
    // Posted: GlobalOptions opt; opt.format = Format::Json; opt.verbose = true;
    // CallRequest req; req.command = "stats";
    // Captured captured;  // CommandsCliTest.cpp:164
    // With both streams dup'd to files:
    //   Printed printed = Call(req, opt, &CaptureExchange, &captured, &client);
    //   Emit(printed, client.exitCode);
    // Restore, then: captured.body is the posted payload (the fake ran),
    // stdout parses as one json envelope whose warnings include code
    // "servitor.verbose" and the redacted request line, stderr is empty
    // and does not contain "servitor request:".
}
```

- [ ] **Step 2: RED.** Full `build.ps1 Debug -Jobs 4` (the desk-cap preamble). The CLI cases compile Servitor sources into ArcaneTests; a test-only edit still needs that project rebuilt, and the preamble applies. Run `.\ArcaneTests.exe "json usage and a verbose posted call leave stderr empty"`. Expected FAIL: the `--json --host` and `--format json --timeout` runs have usage text on stderr from `PrintUsage` (`Client.cpp:471`), because `out.format` is still the Human default. The verbose half fails when captured stderr contains `servitor request:` from `ARC_INFO` (`Client.cpp:560`). Record the seed.

- [ ] **Step 3: Implement the split, and pre-scan argv.** `PrintUsage` takes the format. When the format is json or ndjson, build the envelope and `Write(stdout, ...)`. When it is human, keep `Write(stderr, UsageText(problem))`. Callers after a successful `ParseArgs` (`Client.cpp:626`, `:630`, `:637`, and the verb bodies) pass `opt.format`. The `!ParseArgs` return at `:618-619` happens before `:227-234` assign `out.format`, so a partially filled `out.json` is not enough: a failure can be the token before `--json` (`--host --json` rejects `--json` as a flag value). Before `ParseArgs`, scan `argv[1]` up to but not including `--`. `--json` selects `Format::Json`. `--format` followed by `json`, `ndjson`, or `human` (`ParseFormat`, `Client.cpp:105-110`) selects that format unless `--json` is also present (`--json` wins, the comment at `:227`). No format flag selects `Format::Human`, the `Client.hpp:57` default, for this early return only. Pass that scanned format into `PrintUsage` at `:618-619`. The successful path's TTY default (`:234`) stays where it is. `--verbose` branches on the format before any log call. Json and ndjson append the warning on the envelope inside `Call`, before `Render`, and do not call `ARC_INFO`.

- [ ] **Step 4: GREEN.** The new case PASS, and the existing human usage case still sees stderr text. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `Write(stderr` and `ARC_INFO` under `Source/Programs/Servitor`. Every json/ndjson path has empty stderr. Human paths stay. List each `Write(stderr` you left, with the format guard in front of it.

- [ ] **Step 6: Servitor.** The Catch2 case posts through `Call`. This step posts the same way against a host this step launches. The verb is `stats frame` (`Verbs.cpp:134`), which is a posted call. A bare `stats` is an unknown verb and never enters `Call`, so it cannot witness the verbose stream. Do not use `status`. `launch` takes no `--client` until Task 21. Capture both streams with `Start-Process` so PowerShell does not turn stderr into an error record.

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$dir = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1'
$stdout = Join-Path $dir 'task20-verbose-stdout.json'
$stderr = Join-Path $dir 'task20-verbose-stderr.txt'
& $s --project ReferenceProject launch runtime --headless -- --fixed-time 0
# record $LASTEXITCODE
$p = Start-Process -FilePath $s -Wait -PassThru -NoNewWindow `
    -ArgumentList @('--project','ReferenceProject','--json','--verbose','--client','grok-review-r1','--host','runtime','stats','frame') `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
# record $p.ExitCode
& $s --project ReferenceProject --client grok-review-r1 --host runtime quit
```

  Expected: launch and quit exit 0, `$p.ExitCode` is 0, `$stderr` is empty, `$stdout` is one json object and does not depend on a line that only existed on stderr. Record the three exit codes and both file paths. Quit even when the stream check fails.

- [ ] **Step 7: Commit.** Suite gate first. The message includes the Step 6 exit codes and both stream paths.

```
Keep json Servitor envelopes on stdout.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 21: launch and status accept --client, and quit --client quits that client's hosts

**Files:**
- Modify: `Source/Runtime/ArcaneRemoteProtocol/src/Arcane/Remote/Descriptor.hpp:23-35` (add `std::string client;`)
- Modify: `Source/Runtime/ArcaneRemoteProtocol/src/Arcane/Remote/Descriptor.cpp` `ToText` (`:42-56`) and `ReadDescriptor` (`:261-272`; a missing `client` is `""`)
- Modify: `Source/Runtime/ArcaneRemote/src/Arcane/Remote/HostSession.hpp` (`Config` near `:80`; add `std::string client;`)
- Modify: `Source/Runtime/ArcaneRemote/src/Arcane/Remote/HostSession.cpp` the descriptor built at `:262-273` and the refresh in `WriteHeartbeat` (`:514-525`)
- Modify: `Source/Client/ArcaneClient/src/Arcane/Host/HostConfig.hpp` and `HostConfig.cpp` (parse `--client`, default `""`)
- Modify: `Source/Programs/ArcaneServer/src/ServerConfig.hpp` and `ServerConfig.cpp` (the same flag)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp` (the `rc` block at `:877`), `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` (the `rc` block at `:1965`), `Source/Programs/ArcaneServer/src/ServerApp.cpp` (the `rc` block at `:191`): assign `rc.client` from the parsed string
- Modify: `Source/Programs/Servitor/src/Servitor/Verbs.cpp:1187-1192` (`UnusedGlobal`'s `--yes` check) and `:1204-1222`
- Modify: `Source/Programs/Servitor/src/Servitor/Launch.cpp` `PlanLaunch` (`:370-429`): append `--client` only when `opt.clientSet`
- Modify: `Source/Programs/Servitor/src/Servitor/Discover.cpp` `StatusData` (`:310-318`) and the quit path (`Verbs.cpp` `QuitVerb` at `:615`)
- Test: `Tests/ArcaneTests/src/Remote/DescriptorTest.cpp`, `LaunchTest.cpp`, `HostSessionTest.cpp`, `CommandsCliTest.cpp`

**Interfaces:**
- `launch` and `status` no longer hit the `--client does not apply` refusal (`Verbs.cpp:1216-1217`).
- An omitted `--client` is not empty on the servitor side. `ParseArgs` stores `DefaultClientId()` when `out.client` is empty (`Client.cpp:225-226`) and leaves `clientSet` false (`:209` is the only place that sets it). Forwarding `opt.client` would stamp every host with `servitor/<version> pid N`. `PlanLaunch` appends `--client` and `opt.client` to `plan.argv` only when `opt.clientSet` is true. It does not write a descriptor. The host creates the descriptor at `HostSession::Start` (`:262`) and `WriteHeartbeat` rewrites that same `m_descriptor` (`:525`); a field Launch wrote would not survive.
- `HostConfig` and `ServerConfig` parse `--client <name>` into a `std::string` that defaults to `""`. The three `HostSession::Config` construction sites copy that string to `rc.client`. `HostSession::Config` stores it. `Start` sets `desc.client = m_config.client` before `WriteDescriptorAtomic`. `WriteHeartbeat` sets `m_descriptor.client = m_config.client` before it writes. `ToText` writes the key. `ReadDescriptor` leaves `""` when the key is missing.
- `status --client NAME` filters inside `StatusData`: when `opt.clientSet`, skip a live host whose `descriptor.client` is not `NAME`. Put `client` on the host object that loop already pushes (`:329`) so the test can read it.
- Produces `std::vector<Arcane::Remote::Descriptor> DescriptorsForClient(const std::filesystem::path& dir, const std::string& client);` in `Discover.cpp` (declare it in `Discover.hpp`). It `ReadDescriptor`s each json in `dir` and returns those whose `client` equals the argument. It does not delete stale files. An empty `client` argument returns an empty vector, so a descriptor with an empty `client` is never a match.
- `quit --client NAME` calls that function before `PostVerb` and before `ScanDirectory`. It sends quit to each match, and refuses (exit 2, json envelope on stdout after Task 20) when more than one matches and `--yes` was not passed. The refusal does not connect. It does not imply `--all`. Combining `--client` with `--pid` on `quit` is a usage error. `--client` on `quit` is this selector, not `X-Remote-Client`. Other verbs keep `--client` as the audit header (`Client.cpp:702`).
- `UnusedGlobal` (`Verbs.cpp:1191-1192`) today returns `"--yes applies only with --all (it confirms a Drive command sent to several hosts)"` whenever `opt.yes && !opt.all`, and `Run` calls it for every verb before `QuitVerb` (`Client.cpp:641`). Narrow that check. `--yes` without `--all` is legal when the verb is `quit` and `opt.clientSet` is true: that `--yes` confirms the multi-match selector above, and it still does not set `opt.all`. Every other verb keeps the refusal. `quit --yes` with `clientSet` false keeps it too. The existing loop in `CommandsCliTest.cpp:603-610` constructs `yes` with `clientSet` false and requires the refusal for every row, including `quit`; that loop stays.

- [ ] **Step 1: Write the failing tests.** Name the case `"omitted --client is not forwarded, and status and quit select the named client"`.

  - `PlanLaunch` (`LaunchTest.cpp:93` is the existing call). `clientSet == false` and `opt.client == DefaultClientId()`: `plan->argv` contains no `"--client"`. `clientSet == true` and `opt.client == "grok-review-r1"`: `argv` contains `"--client"` immediately followed by `"grok-review-r1"`, and does not contain `DefaultClientId()`.
  - `HostSession` (`HostSessionTest.cpp:50` is the existing start). `cfg.client` empty: after `Start` and again after `DrainFrame(1)` (that refresh is `WriteHeartbeat`), `ReadDescriptor(...).client` is `""`. `cfg.client == "grok-review-r1"`: both reads return that string.
  - `StatusData` (`Discover.hpp:83`) on a `Scan` the test fills with two `LiveHost`s, pids 11 and 12, clients `"grok-review-r1"` and `""`. `opt.clientSet == true` and `opt.client == "grok-review-r1"`: `hosts.size() == 1` and that pid is 11. `clientSet == false`: both hosts.
  - Two descriptor files in a temp dir, one client `"grok-review-r1"` and one `""` (required keys set so `ReadDescriptor` succeeds: protocol, hostKind, pid, port, token). `DescriptorsForClient(dir, "grok-review-r1").size() == 1`. `DescriptorsForClient(dir, "").empty()`. A second dir with two files that both use `"grok-review-r1"` and distinct pids:
    - `QuitVerb` with `clientSet`, that project, and no `--yes` returns 2 and does not post. A file with client `""` in that dir is not a match.
    - The same `QuitVerb` with `yes` true, `all` false, and `clientSet` true does not return `"--yes applies only with --all"` and posts quit to each match. `UnusedGlobal(*FindVerb("quit"), opt)` on that `opt` returns `nullopt`.
    - `UnusedGlobal` on `quit` with `yes` true, `all` false, and `clientSet` false still returns the `--yes applies only with --all` string. The same options on a verb that is not `quit` (use `status`) still return that string. `Run` of `quit --yes` with neither `--client` nor `--all` exits 2 with that text. After Task 20 the json form of that refusal is one envelope on stdout.
  - Parse `launch --client grok-review-r1` and `status --client grok-review-r1` through the argv helper. The error string does not contain `does not apply`. A descriptor round-trip writes `client == "grok-review-r1"` and reads it back; a file without the key reads `""`.

- [ ] **Step 2: RED.** Rebuild and run the new cases. Expected FAIL: the cases that name `client` or `DescriptorsForClient` do not compile, and `launch --client` returns exit 2 with `does not apply` (`Verbs.cpp:1217`) if that case was built alone. Quote the first compiler line and, when the exe ran, the seed. A green parse check with no `PlanLaunch` / `StatusData` / `QuitVerb` assertion is not this step.

- [ ] **Step 3: Implement the field, the host parse, the three `rc.client` assignments, `PlanLaunch`'s explicit-only append, the `StatusData` filter, `quit --client`, and the `UnusedGlobal` exemption.** `HostSession` writes and refreshes `client`. `Launch.cpp` does not write descriptor JSON. Old descriptors stay readable. At `Verbs.cpp:1191-1192`, return the `--yes applies only with --all` string only when `opt.yes && !opt.all` and the row is not `quit` with `opt.clientSet`. Do not treat that `--yes` as `--all`.

- [ ] **Step 4: GREEN.** Every case in Step 1 PASSes, including the omitted-client `PlanLaunch`, the `HostSession` refresh, `StatusData`'s single pid, `QuitVerb`'s exit 2 without `--yes`, and `QuitVerb`'s posts with `--yes` and not `--all`. The loop at `CommandsCliTest.cpp:603-610` still refuses `quit --yes` when `clientSet` is false. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `does not apply` under `Servitor/`. `launch` and `status` are gone from the `--client` refusal. `--follow` still refuses `--client` (`Verbs.cpp:255-257`) because a stream sends no header. Leave that refusal. List it.

- [ ] **Step 6: Commit.** Suite gate first.

```
Let launch, status, and quit select a Servitor client.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 22: Idle-exit on every Servitor-armed host, and an opt-in owner pid

**Files:**
- Modify: `Source/Client/ArcaneClient/src/Arcane/Host/HostConfig.hpp` and `HostConfig.cpp` (parse `--owner-pid`, default 0)
- Modify: `Source/Programs/ArcaneServer/src/ServerConfig.hpp` and `ServerConfig.cpp` (the same flag)
- Modify: `Source/Runtime/ArcaneRemote/src/Arcane/Remote/HostSession.hpp` (`Config` near `:80`; add `std::uint32_t ownerPid = 0;`) and `HostSession.cpp` the descriptor at `:262-273`, the refresh in `WriteHeartbeat` (`:514-525`), and the tick at `:361`
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp` the `rc` block at `:877` (assign `rc.ownerPid`; the idle line is `:883`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` the `rc` block at `:1965` (assign `rc.ownerPid`; the idle line is `:1972`)
- Modify: `Source/Programs/ArcaneServer/src/ServerApp.cpp` the `rc` block at `:191` (assign `rc.ownerPid`). Do not edit `:196`. It already sets `idleExitSeconds` when `frames == 0`. The sibling sweep lists it and leaves it.
- Modify: `Source/Runtime/ArcaneRemoteProtocol/src/Arcane/Remote/Descriptor.hpp` (add `std::uint32_t ownerPid = 0;`) and `Descriptor.cpp` `ToText` / `ReadDescriptor`
- Modify: `Source/Programs/Servitor/src/Servitor/Launch.cpp` only to keep from appending `--owner-pid` when the operator did not pass it. It does not write the descriptor.
- Test: `Tests/ArcaneTests/src/HostConfigTest.cpp`, `Remote/HostSessionTest.cpp`, `Remote/DescriptorTest.cpp`, `Remote/LaunchTest.cpp`

**Interfaces:**
- `--owner-pid` is opt-in only. Omitted means 0. 0 means the host does not watch any pid. Servitor does not substitute its parent, its own pid, or the shell's pid. This overrides `diagnosis.md`. The 600 s idle backstop is unchanged: do not edit `HostConfig.hpp:400` or `ServerConfig.hpp:42`, and do not add an idle flag.
- `HostConfig` and `ServerConfig` store the parsed `uint32`. The three construction sites copy it: `rc.ownerPid = m_config.ownerPid` in `RuntimeApp.cpp`'s block at `:877` and `EditorAppFrame.cpp`'s block at `:1965`, and `rc.ownerPid = m_cfg.ownerPid` in `ServerApp.cpp`'s block at `:191`. `HostSession::Config` holds it. `Start` sets `desc.ownerPid` before `WriteDescriptorAtomic` (`:262`). `WriteHeartbeat` sets `m_descriptor.ownerPid` from the config before it writes (`:525`). `Launch.cpp` does not build that JSON. A missing key reads as 0.
- When `ownerPid != 0` and `Platform::IsProcessLive(ownerPid, 0)` is false, `DrainFrame`'s tick (`:361`) takes the same exit `IdleExpired` takes (`m_exitReason = "remote-idle"`). A dead owner is an exit, not a hang. `ownerPid == 0` does not take that branch. `IsProcessLive` treats `ERROR_ACCESS_DENIED` as live (`Process.cpp:180-186`), so a test pid has to be one that call returns false for.
- Idle seconds: `RuntimeApp.cpp:883` becomes `rc.idleExitSeconds = (m_config.maxFrames == 0) ? m_config.remoteIdleExitSeconds : 0u;`. `EditorAppFrame.cpp:1972` becomes `rc.idleExitSeconds = (m_config.maxFrames == 0) ? m_config.remoteIdleExitSeconds : 0u;`. Drop the `headless &&` conjunct. A `--frames N` run still uses 0. An interactive editor with no `--remote` never builds this `HostSession`, so it still does not exit. An armed editor with no traffic for 600s does. `--remote-idle-exit 0` still disables it (`IdleExpired` returns false when the seconds are 0, `HostSession.cpp:458`).
- Servitor `launch` of editor, runtime, and server does not pass a new idle flag. `PlanLaunch` appends `--owner-pid` only when the operator's argv already contained it. Host args are already copied through (`Launch.cpp:372`); do not add a token the operator did not type.

- [ ] **Step 1: Write the failing tests.** Name the case `"omitted --owner-pid is zero, and a dead owner pid exits"`.

```cpp
TEST_CASE("omitted --owner-pid is zero, and a dead owner pid exits", "[host][remote]") {
    // HostConfig::Parse of { "ArcaneEditor", "--remote" } yields ownerPid == 0.
    // HostConfig::Parse of { "ArcaneEditor", "--remote", "--owner-pid", "4242" } yields 4242.
    // ServerConfig::Parse of the same two argvs yields 0 and 4242.
    // A descriptor written without ownerPid reads 0.
    //
    // HostSession, ownerPid 0: Start succeeds, ReadDescriptor.ownerPid == 0,
    // DrainFrame(1) returns false, ExitReason() is nullptr, and a second
    // ReadDescriptor (the heartbeat refresh) is still 0.
    //
    // HostSession, dead pid: walk uint32 candidates down from 0x0FFFFFF0
    // until Platform::IsProcessLive(candidate, 0) is false. That value is
    // not 0. Start, ReadDescriptor.ownerPid equals it, DrainFrame(1)
    // returns true, ExitReason() is "remote-idle".
    //
    // PlanLaunch with host args that do not contain "--owner-pid": argv does
    // not contain it. PlanLaunch with those two tokens in host args: they
    // are still there, and no second pair was inserted.
}
```

  Source pin in the same test file: the `idleExitSeconds` assignment in `RuntimeApp.cpp` (`:883`) does not contain `headless`, and the assignment in `EditorAppFrame.cpp` (`:1972`) does not contain `headless`. Each of the three `rc` blocks (`RuntimeApp.cpp:877`, `EditorAppFrame.cpp:1965`, `ServerApp.cpp:191`) contains `ownerPid`. `ServerApp.cpp`'s idle assignment still keys off `frames == 0` and is not rewritten. Do not add a fake clock. Do not point a pid at the test process or the shell.

- [ ] **Step 2: RED.** Rebuild and run the case. Expected FAIL: `--owner-pid` is an unknown flag (exit 2 / parse error), or `ownerPid` is not a field. Record the seed.

- [ ] **Step 3: Implement the flag, the three `rc.ownerPid` assignments, the descriptor key `HostSession` writes and refreshes, the dead-pid watch, and the two idle assignments.** Do not default the pid. Do not change 600. `Launch.cpp` does not write descriptor JSON.

- [ ] **Step 4: GREEN.** The new cases PASS, including the dead-pid `DrainFrame` and the omitted-pid `0` across `Start` and the heartbeat refresh. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `idleExitSeconds` and `ownerPid` under `Source/Programs`. Server already keys idle off `frames == 0`. Editor and runtime key idle off `maxFrames == 0` after this task. All three construction sites assign `ownerPid`. List any third host that still requires `headless`.

- [ ] **Step 6: Servitor evidence.** Task 21 has landed, so `launch`, `status`, and `quit` take `--client`. Do not pass `--owner-pid`. Do not point it at the shell.

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$status = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1\task22-status.json'
& $s --project ReferenceProject --client grok-review-r1 launch runtime --headless -- --fixed-time 0
& $s --project ReferenceProject --client grok-review-r1 --host runtime wait settled
& $s --project ReferenceProject --client grok-review-r1 status --client grok-review-r1 | Out-File -Encoding utf8 $status
& $s --project ReferenceProject --client grok-review-r1 quit --client grok-review-r1
```

  Record every command, its exit code, and `$status`. The status json has `"ownerPid": 0` or no `ownerPid` key. Quit even when the check fails.

- [ ] **Step 7: Commit.** Suite gate first.

```
Idle-exit armed hosts and watch an owner pid only when asked.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 23: Servitor skill and the repo Servitor sections match the CLI

**Files:**
- Modify: `.claude/skills/arcane-servitor/SKILL.md`
- Modify: `CLAUDE.md` (the Servitor section)
- Modify: `AGENTS.md` (the Servitor section)
- Modify: the one sentence in `docs/superpowers/specs/2026-10-05-arc-cli-design.md` that says a json failure is written to stderr. Grep `stderr` in that file and change that sentence so json and ndjson failures are on stdout. Do not rewrite the spec.

**Interfaces:**
- The skill gains two wrappers, PowerShell 5.1 and bash. Both `try/finally` or `trap` a `quit` of the host they launched, and both branch on the exit code (0, 2, 3, 4, 5 retry, 6, 7, 8). They pass `--client` on `launch` and on `status`. They do not pass `--owner-pid`. They do not default a pid. Paths use `D:\dev\starworks\Arcane-fixes` in the PowerShell sample and the repo-relative binary in the bash sample.
- `CLAUDE.md` and `AGENTS.md` stop saying `launch` and `status` refuse `--client`. They say `launch` and `status` accept `--client`, `quit --client <name>` quits that client's hosts, json and ndjson stay on stdout with stderr empty, and `--owner-pid` is opt-in. The exit-code list stays.

- [ ] **Step 1: Read the current Servitor sections and the skill's launch loop.** Quote the sentence that says `launch` and `status` refuse `--client`. That sentence is the RED. There is no Catch2 case. The check is `Select-String -Path CLAUDE.md,AGENTS.md, .claude\skills\arcane-servitor\SKILL.md -Pattern 'refuse'` returning no Servitor hit after the edit, and both wrappers present (`finally` in the PowerShell block, `trap` in the bash block).

- [ ] **Step 2: Edit the three docs and the one spec sentence.**

- [ ] **Step 3: Re-grep.** Expected: no "refuse `--client`" sentence remains in those Servitor sections. The wrappers contain `quit` and do not contain `owner-pid`.

- [ ] **Step 4: Sibling sweep.** Grep `refuse` and `--client` under `docs/` and `.claude/skills/`. Update a second copy of the refused-client sentence if one exists. List docs that mention Servitor and were left unchanged, with why.

- [ ] **Step 5: Commit.**

```
Document the Servitor client, stdout envelope, and opt-in owner pid.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

### Task 24: The InfoQueue1 QueryInterface warning logs once per process

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/DeviceCreationD3D12.cpp:744-775`
- Test: `Tests/ArcaneTests/src/DeviceCreationWarnOnceTest.cpp` (new) or an existing D3D12 creation test if one already includes this cpp's text

**Interfaces:**
- The `ARC_WARN` at `:765` runs only the first time this process takes that else-branch. `CreateD3D12NativeDevice` (`:440`) is the function that contains it, and it returns `bool`. Queue creation is later in the same function (`:784-791`). A `return` from the guard skips `CreateCommandQueue` on every later device.
- Guard the warn only:

```cpp
static std::atomic<bool> logged{false};
if (!logged.exchange(true)) {
    ARC_WARN("ID3D12InfoQueue1 QueryInterface failed" /* the existing arguments */);
}
```

  The first failure still logs the `hr` values and the `why` string. Later devices do not log that sentence. Execution falls through to `CreateCommandQueue` on every call, including the ones that skip the warn. Do not `return` from `CreateD3D12NativeDevice` in this guard.
- Do not edit `ThirdParty/NRI`.

- [ ] **Step 1: Write the source pin.**

```cpp
TEST_CASE("the InfoQueue1 QueryInterface warning is once per process", "[render][d3d12]") {
    const auto text = ReadRepoFile("Source/Client/ArcaneRender/src/Arcane/Render/DeviceCreationD3D12.cpp");
    const auto warn = text.find("ID3D12InfoQueue1 QueryInterface failed");
    REQUIRE(warn != std::string::npos);
    const auto before = text.substr(warn > 400 ? warn - 400 : 0, 400);
    CHECK(before.find("exchange") != std::string::npos);
    CHECK(before.find("return") == std::string::npos);
    const auto queue = text.find("CreateCommandQueue", warn);
    CHECK(queue != std::string::npos);
}
```

  The 400 characters before the warn today are the `why` ladder (`:746-764`) and contain no `return`. `CreateCommandQueue` at `:786` is the fall-through the guard must keep.

- [ ] **Step 2: RED.** Rebuild ArcaneTests. Run the case. Expected FAIL: `exchange` is absent in the 400 characters before the warn. Record the seed.

- [ ] **Step 3: Add the atomic around the warn only.** Include `<atomic>` if the cpp does not already. Do not return.

- [ ] **Step 4: GREEN.** The pin PASS, including `return` absent before the warn and `CreateCommandQueue` still after it. Record the seed.

- [ ] **Step 5: Sibling sweep.** Grep `ARC_WARN` in `DeviceCreationD3D12.cpp`. The other warns in this function (`PushStorageFilter` near `:688`, `RegisterMessageCallback` near `:728`, debug layer not active near `:739`) are per device today. List them as `Left as-is:` unless one is the same QueryInterface sentence duplicated. Do not once-flag a warn this task was not given unless it is that same sentence.

- [ ] **Step 6: Servitor.** One host cannot create two D3D12 devices, so this step does not claim a second creation. `NriGraphContext.hpp:90-95` states the rule: one device per process, the host-window context creates it, the offscreen context borrows it, and a second device is the topology the design excludes (one `VkDevice` on the default dispatcher, one flip-model swapchain per HWND). `CreateOffscreen` is `BORROWED. No NativeDeviceOwner, no Wrap` (`NriGraphContext.cpp:339-346`). A project switch keeps the chrome context, which owns the process's only graphics device, and destroys only the viewport context (`EditorAppProject.cpp:2498-2506`). `g_d3d12DeviceCreated` is set at `DeviceCreationD3D12.cpp:625` and never cleared (`:61`, `:482`). Vulkan does not enter `CreateD3D12NativeDevice`. A test that calls `NativeDeviceOwner::Create` twice in one process is the topology that comment excludes; do not add that behavior test. The once-per-process proof that sees both creations is the Step 1 pin: `exchange` in the 400 characters before the warn, no `return` in that window, and `CreateCommandQueue` still after the warn, so a later call in the same process skips the sentence and still builds the queue. The live check is one device's log. Count 0 is a pass when the QueryInterface succeeds. Two or more copies are a failure. Task 21 has landed, so `launch` takes `--client`. Pass `--backend dx12` so the host takes the D3D12 path. Golden gate is not required.

```powershell
$s = 'D:\dev\starworks\Arcane-fixes\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$logs = 'D:\dev\starworks\Arcane-fixes\test-results\review-round-1\task24-warn.json'
& $s --project ReferenceProject --client grok-review-r1 launch runtime --headless --backend dx12 -- --fixed-time 0
& $s --project ReferenceProject --client grok-review-r1 --host runtime wait settled
& $s --project ReferenceProject --client grok-review-r1 --host runtime logs --level warn | Out-File -Encoding utf8 $logs
& $s --project ReferenceProject --client grok-review-r1 --host runtime quit
```

  Record every command, its exit code, `$logs`, and the count of `ID3D12InfoQueue1 QueryInterface failed` in that file. Expected: launch, wait, and quit exit 0, and the count is 0 or 1. Quit even when the count is wrong.

- [ ] **Step 7: Commit.** Suite gate first. The message includes the Step 6 exit codes, the log path, and the warning count.

```
Log the InfoQueue1 QueryInterface miss once per process.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

---

## Self-review

- Coverage: rank 1 is Tasks 1–4. Rank 2 is Task 5. Rank 3 is Task 6. Rank 4 is Task 7. Rank 5 is Task 8. Rank 6 is Task 9. Rank 7 is Task 10. Rank 11 is Tasks 11–12. Rank 13 is Tasks 13–16. Rank 14 is Task 17. The held screenshot is Tasks 18–19. Servitor friction is Tasks 20–23. The InfoQueue1 warn is Task 24. Each task has a sibling-sweep step.
- Review Focus pins sit in Tasks 1, 3, 4, 6, 11, 12, 18, 20, 21, and 22. The case names in those tasks are the names in Review Focus.
- Tasks 5, 6, 7, 17, 20, and 24 each record the Servitor command, its exit code, and the artifact path. Tasks 5 and 6 drive the competing modals and the component-gone gizmo abandon through Dist-stripped commands and `quit` the editor. Task 7 arms the failed restore and `quit`s the editor, which stays up. Task 17's server and runtime exit before ready (exit 7); a launch that returns 8 is quit by pid. Task 24 states the one-device limit, keeps the source pin, and `quit`s the one dx12 runtime. Tasks 18, 19, and 22 record the same evidence and `quit` the host they launched.
- No step says "similar to task N". No step says TBD. Baseline integers for unfiltered lanes are measured in Task 14, not invented. The hosted Dist lane is an exemption with a reason, because this desk must not set `CI`.
- `ReadRegularFile`, `PathIsInsideDirectory`, `ModalAllowed`, `CommandStack::Abandon`, `PlaySession::FailNextRestoreForTesting`, `ClearSceneReferences` returning `bool`, `ArtifactStore::RemoveAllExcept`, `ProcessContext::AcquireJobSystem`, `PresentDerivedState`, `Descriptor::client`, and `Descriptor::ownerPid` are the names later tasks use. They match the task that produces them.
- `Cancel` is not given new behavior. `~JobSystem` nulls the executor through the concrete `shared_ptr`, not `dynamic_cast`.
