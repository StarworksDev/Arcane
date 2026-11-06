# 3D-readiness Plan C — GPU Resources Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Execution gates (re-vet 2026-10-19).** Task 2 waits for fix-round Task 7, and Task 6 waits for fix-round Tasks 11-12, in addition to this plan's own gate (Plans A and B merged). Run each plan's Servitor sequence once on an unmodified base build first.

**Goal:** Give bindless slots and resident meshes and textures one device-wide lifetime, one generated capacity, and one residency account, and record the fix #11 completion check that says whether the arc is finished.

**Architecture:** Task 1 checks in `bindlessCapacity = 256` through a generator both build scripts run before project generation and before shader compile. Task 2 replaces the add-only per-context table with one free list per device, recycled only after every sampling context's fence has retired. Task 3 measures resident bytes and only then changes a provisional default. Task 4 is the account (slices, borrowing, admission). Task 5 is the upload node (`CopyDst`, then the consumer's read). Task 6 is the hidden fix #11 check plus the last golden gate. This plan does not implement fix #11.

**Tech Stack:** C++23, HLSL via DXC, NRI (D3D12 + Vulkan), premake5, Catch2 v3, PowerShell. The NONE backend's upload alignments are 1 and 1; the footprint test injects 256 and 512 and does not read that pair.

**Spec:** `D:/dev/starworks/arcane-readiness/2026-10-17-3d-readiness-arc-design.md`, sections 3.5, 3.6, 4 steps 0, 2, 7, and 8, Revision 2, Revision 3, and the Revision 4 line. Revision 3 overrides Revision 2's "round slicePitch to 16" and "`capacity + 1`". Revision 4 overrides any raw-`remaining` fit test: capacity is a byte range with no step, and the fit is `Align(cursor, alignment) + alignedBytes <= capacity` through one helper shared with `Allocate`.

**Prerequisite gate:** Plans A and B are merged. `build/abi-digest.txt` exists. `kGamePluginABIDigest` is checked at load. Do not bump `kGamePluginABIVersion` in this plan. `RenderTextureSettings` and `RenderResidencySettings` go in `RenderBudgetSettings.hpp`, which is **not** one of the 21 `kGameFacingHeaders`. Do not add that header to the manifest list. `build/abi-digest.txt` stays put. If a review forces the header onto the game-facing list, update the digest in that same commit and still do not bump the epoch.

Fix #11 (one `JobSystem` on `ProcessContext`, destructor order `JobSystem.cpp:119-126`) is not a prerequisite of Tasks 1–5. Fix #11 is fix-round Tasks 11-12. Run Task 6 after both are on `main`. It then takes the accessor branch and passes without `[.]`. The arc is not complete until that test is green without the `[.]` hide.

## Global Constraints

- **Worktree:** `D:\dev\starworks\Arcane-3dr`, branch cut from `main` `a12f33e6`. `Arcane-f5` is the old F5 branch at `309aa06e`; never build or test there. Never push, never force-push, never merge to `main`.
- **One heavy build at a time.** Do not start `build.ps1`, `msbuild`, or `cl.exe` while another worktree on this machine is already compiling. The same cap applies to `[gpu]` runs: one GPU suite at a time. Desk build, from the repo root. Windows PowerShell 5.1 `Start-Process` has no `-Priority`. `-PassThru` returns the process; `PriorityClass` is how 5.1 lowers it. `ExitCode` is the build's code.

```powershell
Remove-Item Env:CI -ErrorAction SilentlyContinue
Remove-Item Env:ARCANE_BUILD_MACHINE -ErrorAction SilentlyContinue
$env:_CL_ = '/MP4'
$build = Start-Process -FilePath powershell.exe -ArgumentList @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',
    'D:\dev\starworks\Arcane-3dr\scripts\ci\build.ps1','Debug','-Jobs','4'
) -WorkingDirectory 'D:\dev\starworks\Arcane-3dr' -PassThru -NoNewWindow
if (-not $build.HasExited) { $build.PriorityClass = 'BelowNormal' }
$build.WaitForExit()
if ($build.ExitCode -ne 0) { throw "desk build failed: exit $($build.ExitCode)" }
```

`build.ps1` always regenerates (`scripts/ci/build.ps1:39-43`). A new `.cpp` is globbed by premake (`premake5.lua:1316`) and is not in the vcxproj until that generate.
- **Tests** run from the exe directory. Record the seed. New cases use `--rng-seed 1`. After any `Set-Location` into that directory, every script path is root-qualified (`D:\dev\starworks\Arcane-3dr\scripts\...`). Suite gate:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

`run-tests.ps1` is `~[gpu]`. `[gpu]` cases are run by name, on D3D12 and on Vulkan, and assert `RenderErrorCount() == 0`. D3D12 cases skip through `ARC_REQUIRE_BACKEND` (`Helpers/GpuCapability.hpp:57`). `test_main.cpp` forces `ForceD3D12DebugLayerForProcess` (PR #16); leave it, and do not set `ARCANE_TESTS_NO_D3D12_DEBUG`. Its listener fails any case whose `RenderErrorTotal()` rose. `ResetRenderErrorCount()` does not hide that rise. A case that trips the latch on purpose is tagged `[expects-render-errors]`, and no case in this plan should need the tag.
- **Red steps.** After writing a new test `.cpp`, run the desk build before `ArcaneTests.exe`. **Compile error:** the build's `ExitCode` is non-zero and the log names the missing declaration. Do not run the previous exe. **Assertion:** the build's `ExitCode` is 0, then the new exe fails a named `CHECK`. A test body that is only a comment compiles and passes; that is not a red.
- **No golden re-bless.** Run the golden gate after Task 1, after Task 2, and after Task 5, and once more in Task 6. The invocation is `powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1`, including after a `Set-Location` into the exe directory. Delete `imgui.ini` in `Binaries\Debug-windows-x86_64-md\ArcaneRuntime` and `ArcaneEditor` before each run. Expected every time: 14 lanes, every `diffCount` 0 (`golden-gate.ps1:326`).
- **Servitor** after Tasks 1, 2, 3, 4, and 5. Task 6 records a hidden in-process observation and does not change host-visible behaviour, so it has no Servitor step. Binary `Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe` from this tree. Own `--headless` runtime. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. Client name `plan-c`. Never drive an editor the user has open. Never `--bless`. Quit what you launched, including when a later command fails. Record every exit code. Exit 0 is the pass. Exit 8 is a pixel miss (`compare.failed`). Exit 7 means the host died.
- **Style:** `/MD`, no `/fp:fast`, ASCII comments, UTF-8 without BOM. Do not edit `ThirdParty/NRI`. A new tunable is a settings field. `ARC_CONSTANT` marks an invariant and expands to nothing (`Constant.hpp:11-14`); `ConstantGuardTest` fails on an unmarked numeric constant. `ApplyMode::Restart` is the enumerator at `CVarTypes.hpp:134` ("read once at boot").
- **Failure** is a refusal or an empty result. No throw on the upload path. Hardware that cannot index the compiled bindless capacity is still refused (`MeshNode::Create`, the check at `MeshNode.cpp` in the 208–221 region; re-read it before editing). No runtime growth. No `VARIABLE_SIZED_ARRAY` (`MeshNode.cpp` records why near the 426 region; re-read before editing).
- **Fences** mean something only inside one context's submission timeline. `NriDevice::Graves()` stays drain-only. Never bury a descriptor or a texture at another context's fence value, and never in `Graves()`.
- **Every commit message ends with:**

```
Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

### Line drift

Re-verified 2026-10-19 on `main` `a12f33e6`: no cited file changed since `309aa06e`.

| Citation | Code |
|---|---|
| `build.ps1` first step | `scripts/ci/build.ps1:39` is `Step 'generate'` of `GenerateProjects.bat`. |
| `build.sh` generate | `scripts/ci/build.sh:87` is the premake `gmake` step. |
| `compile-shaders.bat` | Line 89 invokes DXC with no `-I`. The include directory is the source file's directory. `bindless-capacity.hlsli` sits in `data/shaders/` beside `mesh.hlsl`. `data/shaders/generated/` stays gitignored. |
| `kBindlessCapacity` | `MeshNode.hpp:605` is `256`. `mesh.hlsl:161` is `#define kMeshBindlessCapacity 256`. The comment is lines 155–160. |
| `BindlessTable` | Policy comment `BindlessTable.hpp:17-20`. `kInvalidSlot` line 55. `Add` line 85. Whole-table `Release` lines 87–93. |
| `RenderGraph` `RgUsage` | `RenderGraph.hpp:149-154`. `ImportBuffer` line 169 takes name, buffer, size only. |
| `StateFor` | `CopyDst` is `RenderGraph.cpp:109-111` (`COPY_DESTINATION` / `COPY_DESTINATION` / `COPY`). Buffer layout forced `UNDEFINED` at lines 125–130. `ShaderRead` is lines 99–101. `kShaderReadStages` is lines 70–71 (vertex, fragment, compute). `kUnknownState` is lines 78–79. Missing-case assert is lines 150–151. |
| `TextureUsageFor` | `RenderGraphExec.cpp:212-226`. `IndirectArgs` is line 224 (no texture bit). Copy usages are lines 220–223. |
| `RingLayout::AlignUp` | `NriUploadRing.cpp:30-35`, ceiling division `((value + align - 1) / align) * align`. `Allocate` uses it at line 62 and rejects at lines 67–70, incrementing `m_overflowCount`. `Reset` zeros the cursor at lines 50–53. |
| `uploadRingBytesPerFrame` | Default `4ull << 20` at `RenderDeviceSettings.hpp:82`. Range `1048576..268435456` (1 MiB..256 MiB) at line 114. No step. `4 MiB + 511` is legal. |
| Mesh budget | `RenderBudgetSettings.hpp:87` default `512ull << 20`. Range at line 93: `67108864..17179869184`. `ARC_SETTINGS(RenderMeshSettings)` is `RenderBudgetSettings.cpp:7`. |
| Latch | `NriMeshBufferCache.cpp:32` reads `Settings<RenderMeshSettings>().residencyBudgetBytes`. |
| `SelectEvictions` | `MeshResidencyBudget.hpp:96-131`. Bails at lines 104–105 when `total <= budget`. Skips `lastDrawnFrame == currentFrame` at line 111. |
| `MeshResidencyBytes` | `MeshResidencyBudget.hpp:76-82`. CPU copy plus GPU buffers, `2 * (vertex + index) + section`. |
| Per-vehicle comment | `MeshResidencyBudget.hpp:32-39`. |
| UploadData call sites | `NriTextureCache.cpp:339` (content pixels), `:454` (checkerboard placeholder), `:582` (mip chain). `NriMeshBufferCache.cpp:221` (VB + IB). All four submit and wait. All four move. |
| `kMaxFramesInFlight` | `FramePacing.hpp:39`, value 3. |
| JobSystem | On `a12f33e6`: `JobSystem.hpp:3-7` says one scheduler per process. `Runtime.cpp:200` is a `JobSystem` member, constructed at `:239`. The destructor at `JobSystem.cpp:119-126` resets the adapter, then the task executor, then calls `WaitforAllAndShutdown`. `ProcessContext.hpp` has no `JobSystem`. Fix Task 11 reorders that destructor (scheduler shutdown before the executor drops). Fix Task 12 moves the `JobSystem` onto `ProcessContext`. Re-read all three after they merge. |

## Review Focus

1. **Recycling a slot while any sampling context's fence is in flight is forbidden**, even when another context's fence has retired. Pinned in Task 2: context A samples and stays in flight, context B releases and allocates, B gets a different index, and the old index returns only after A's fence completes. The `[gpu]` pair keeps `RenderErrorCount() == 0`.
2. **Admission does not stop because the mesh fits its 512 cap.** Textures at 700 and a new 512 mesh would sum to 1212 if the code returned early on `512 <= 512`. The locked result is mesh 512, texture 512, sum 1024, T1 evicted, T2 and M resident. Pinned in Task 4.
3. **Texture footprint uses row and slice alignment, not "round slicePitch to 16" and not the mip-0 `(alignedSize * 4) / 3` guess.** Pinned numbers are in Task 5. A 1×1 RGBA8 result of 16 fails.
4. **Ring fit is the helper `Allocate` uses, including a non-aligned capacity.** `AlignUp(cursor, alignment) + alignedBytes <= capacity` is the readable form. The implementation is the subtraction form at `NriUploadRing.cpp:67-68` so a huge `size` cannot wrap the add. Capacity `4 MiB + 511`, cursor `4 MiB - 16`, alignment 512, does not fit, and `Allocate` agrees. Pinned in Task 5.
5. **A payload larger than the slot is dedicated staging in the same frame, not a retry, and it does not bump the ring overflow count.** Mesh: vertex region `capacity`, index region 16, `alignedBytes = capacity + 16`. Texture under injected alignments 256 and 512: mip footprints sum to `capacity + 512`. Classifying either as "retry next frame" fails. A drawn-this-frame entry is not evicted when a different vehicle asks. Pinned in Tasks 4 and 5.

## Out of scope

- Implementing fix #11. Task 6 only detects it.
- Raising `bindlessCapacity` above 256. The generator makes a later raise a one-line lua edit.
- A copy queue, unbounded descriptor arrays, bindless paging, ImGui's descriptor pools.
- Box3D, F5 Plans 2 and 3, an ABI epoch bump, a golden re-bless.
- Editing `ThirdParty/NRI`, including `HelperInterface.hpp` and `SharedExternal.h`. Call their alignment rules. Do not change them.
- L09 and L08.

## File map

| File | Task | Responsibility |
|---|---|---|
| `build/engine-constants.lua` (new) | 1 | `bindlessCapacity = 256`. |
| `scripts/generate-engine-constants.ps1` and `.sh` (new) | 1 | Write the two outputs and nothing else. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp` (new, checked in) | 1 | `kBindlessCapacity`. |
| `data/shaders/bindless-capacity.hlsli` (new, checked in) | 1 | `kMeshBindlessCapacity`. |
| `scripts/generate.bat`, `scripts/generate.sh` (new), `data/shaders/compile-shaders.bat`, `data/shaders/compile-shaders.sh`, `scripts/ci/build.ps1`, `scripts/ci/build.sh` | 1 | Each entry point runs the generator. CI uses `--check` / `-Check` and does not write the committed files first. |
| `data/shaders/mesh.hlsl` | 1 | `#include "bindless-capacity.hlsli"` instead of the literal. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/BindlessTable.hpp` | 2 | Free list. One table per device. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/DeviceResourceLifetime.hpp` (new) | 2 | Device-wide last-use record. Bindless and residency both use it. |
| `RenderBudgetSettings.hpp`, `RenderBudgetSettings.cpp` | 3 | The two new settings, after the measurement. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/ResidencyAccount.hpp` + `.cpp` (new) | 4 | Caps, surplus, admission. |
| `NriUploadRing.cpp`, `RenderGraph.hpp`, `RenderGraph.cpp`, `RenderGraphExec.cpp` | 5 | Fit helper, `VertexBuffer` / `IndexBuffer`, upload node. |

---

### Task 1: Generated bindless capacity

**Files:**
- Create: `build/engine-constants.lua`
- Create: `scripts/generate-engine-constants.ps1`
- Create: `scripts/generate-engine-constants.sh`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp`
- Create: `data/shaders/bindless-capacity.hlsli`
- Modify: `scripts/ci/build.ps1` (insert a step before line 39)
- Modify: `scripts/ci/build.sh` (insert a step before line 87)
- Modify: `data/shaders/mesh.hlsl:161`
- Modify: `MeshNode.hpp:604-605` to include the generated header instead of the literal
- Test: `Tests/ArcaneTests/src/EngineConstantsTest.cpp` (new)

**Interfaces:**
- Consumes: nothing from Plans A or B except a green tree.
- Produces: `Arcane::kBindlessCapacity` in the generated header, and `kMeshBindlessCapacity` in the hlsl include. Both equal the lua field. The script writes only those two paths. It is not the ABI digest tool.

`build/engine-constants.lua` is one assignment:

```lua
bindlessCapacity = 256
```

The PowerShell script reads that integer and writes:

```cpp
#pragma once
#include <cstdint>
#include <Arcane/Core/Constant.hpp>
namespace Arcane {
    ARC_CONSTANT("shader/GPU contract: bindless material table capacity; source of truth is build/engine-constants.lua")
    inline constexpr std::uint32_t kBindlessCapacity = 256;
}
```

`ARC_CONSTANT` expands to nothing (`Constant.hpp:11-16`). The marker is the immediately previous non-empty line (`ConstantScan.hpp:166-171`). No blank line between the marker and the declaration. The scanner walks `Source/Client` (`ConstantScan.hpp:139-179`), so `Generated/BindlessCapacity.gen.hpp` is scanned. `ConstantGuardTest.cpp:170-178` fails an unmarked numeric constant. Both generators emit this header, including `Constant.hpp`. HLSL has no `ARC_CONSTANT`; the guard scans C++ only. Do not add an allow-list entry. Run `ConstantGuardTest` (the suite, or that case) before Task 1 commits.

and:

```hlsl
#ifndef ARC_BINDLESS_CAPACITY_HLSLI
#define ARC_BINDLESS_CAPACITY_HLSLI
#define kMeshBindlessCapacity 256
#endif
```

`MacroPrefixGuardTest` scans `data/shaders/*.hlsli`, so no macro in this file is spelled `ARCANE_*` (house form: `ARC_GPU_SCENE_HLSLI`).

with the integer taken from the lua file, not typed twice in the script. The shell script writes the same bytes (LF). The checked-in hpp uses LF. `mesh.hlsl` replaces the `#define` at line 161 with `#include "bindless-capacity.hlsli"`. Delete the sentence that says the comment is the whole of the contract (`mesh.hlsl:155-160` and the mirror at `MeshNode.hpp:598-603`). Replace it with: the value comes from `build/engine-constants.lua`; the generated files are checked in; a drift test fails if a rerun would change them.

`MeshNode.hpp` includes `BindlessCapacity.gen.hpp` and deletes the local `static constexpr` at line 605. Call sites of `MeshNode::kBindlessCapacity` become `Arcane::kBindlessCapacity`. Keep the symbol available as `MeshNode::kBindlessCapacity` only if it is an alias of the generated constant, so `RenderGraphTest.cpp`'s pool-size math (the comment at `MeshNode.hpp:590-592`) still compiles:

```cpp
static constexpr std::uint32_t kBindlessCapacity = Arcane::kBindlessCapacity;
```

The script exits non-zero if the lua field is missing or not an integer in `1..4096`. It does not run premake and it does not invoke DXC.

Wire it at every entry that generates projects or compiles shaders. `GenerateProjects.bat` is a wrapper to `scripts\generate.bat` (lines 1–3). `scripts/generate.bat` calls premake at line 56. `data/shaders/compile-shaders.bat` sets DXC at line 31 and starts compiles at line 39 with no `-I`; includes resolve beside the source. `compile-shaders.sh` parses the bat for `call :compile` and does not execute a powershell line at the top of the bat, so it needs its own call. `premake5.lua:370-372` prebuildcommands call both the bat and the sh. There is no `scripts/generate.sh` today; Linux generation is `scripts/ci/build.sh:87` calling premake directly.

- `scripts/generate.bat`: run `generate-engine-constants.ps1` before the premake call on line 56.
- Add `scripts/generate.sh` as the Linux twin of `generate.bat`. It runs `generate-engine-constants.sh`, then the same premake `gmake` line `build.sh` uses today. `scripts/ci/build.sh` calls `scripts/generate.sh` instead of premake directly.
- `scripts/ci/build.ps1`: run the PowerShell generator before the generate step on line 39.
- `data/shaders/compile-shaders.bat` and `data/shaders/compile-shaders.sh`: run the matching generator before DXC.

Two modes. With no flag, the script writes the two checked-in paths. A developer who changed the lua runs that mode and commits. With `-Check` (PowerShell) or `--check` (shell), the script regenerates both outputs in a temp directory and exits non-zero if either differs from the checked-in file. It does not write the committed files first. CI uses only the check mode. A second check leaves `git diff` empty. The Catch2 test invokes the check (or replicates that temp-dir compare) and also checks `kBindlessCapacity == 256`. It does not run the writer against the committed paths before asserting.

- [ ] **Step 1: Write the failing drift test**

```cpp
TEST_CASE("generated bindless capacity matches engine-constants.lua", "[render][constants]")
{
    const auto root = Arcane::Test::RepoRoot();
    const std::string lua = ReadFileBinary(root / "build/engine-constants.lua");
    REQUIRE(lua.find("bindlessCapacity = 256") != std::string::npos);
    const std::string hpp = ReadFileBinary(root / "Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp");
    const std::string hlsl = ReadFileBinary(root / "data/shaders/bindless-capacity.hlsli");
    CHECK(hpp.find("kBindlessCapacity = 256") != std::string::npos);
    CHECK(hlsl.find("kMeshBindlessCapacity 256") != std::string::npos);
    const std::string mesh = ReadFileBinary(root / "data/shaders/mesh.hlsl");
    CHECK(mesh.find("#include \"bindless-capacity.hlsli\"") != std::string::npos);
    CHECK(mesh.find("#define kMeshBindlessCapacity") == std::string::npos);
    CHECK(Arcane::kBindlessCapacity == 256u);
}
```

- [ ] **Step 2: Build, then stop on the compile error**

The test calls `Arcane::kBindlessCapacity` and includes the generated header, which does not exist yet. Desk build. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `kBindlessCapacity` or the missing header. Do not run `ArcaneTests.exe`. That compile error is the red. A missing file observed by the old exe is not this run.

- [ ] **Step 3: Add the generator and the checked-in outputs**

Write the scripts and the two outputs, including the `ARC_CONSTANT` marker and the `Constant.hpp` include. Wire `generate.bat`, the new `generate.sh`, both compile-shaders scripts, `build.ps1`, and `build.sh` as the Interfaces section says. Point `mesh.hlsl` and `MeshNode.hpp` at the outputs. Run the writer once. Then run `-Check` / `--check`. Expected: exit 0, and `git diff` empty on the two outputs. The check must not have written them in order to make them match.

- [ ] **Step 4: Build, test, shaders, golden**

Desk build. Expected: exit 0, and `compile-shaders.bat` (ArcaneRender's prebuild, `premake5.lua:370`) compiles `mesh.hlsl` with the include. A DXC error that the include is missing means the hlsli is not beside `mesh.hlsl`.

```powershell
.\ArcaneTests.exe "generated bindless capacity matches engine-constants.lua" --rng-seed 1
```

Expected: PASS. From the exe directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0, including `ConstantGuardTest` (the generated header is marked).

Golden gate, Debug, both `imgui.ini` deleted. The script path is root-qualified:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1
```

Expected: 14/14, `diffCount` 0. No `--bless`.

Run this exact Servitor sequence once on an unmodified build of the arc's base commit and record the exit codes. `compare runtime-scene` after `wait settled` reads the held frame 0. Fix-round Tasks 18-19 change what frame 0 presents. If the base tree already exits 8, that is pre-existing and not this task's regression. Report it and do not bless.

Servitor. Capacity stays 256, so the reference picture should match. Own headless runtime. Record every exit code. Quit even if a later command fails.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-c --host runtime wait settled
& $S --project ReferenceProject --client plan-c --host runtime compare runtime-scene --source window
& $S --project ReferenceProject --client plan-c --host runtime quit
```

Expected: every exit 0. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. `compare` exit 8 is a pixel miss. Do not bless.

- [ ] **Step 5: Commit**

```
git add build/engine-constants.lua scripts/generate-engine-constants.ps1 scripts/generate-engine-constants.sh scripts/generate.bat scripts/generate.sh scripts/ci/build.ps1 scripts/ci/build.sh data/shaders/compile-shaders.bat data/shaders/compile-shaders.sh Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp data/shaders/bindless-capacity.hlsli data/shaders/mesh.hlsl Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.hpp Tests/ArcaneTests/src/EngineConstantsTest.cpp
git commit -m "build: generate the bindless capacity from one lua field"
```

with the trailer.

---

### Task 2: Bindless free list and the device-wide last-use record

**Prerequisite of this task:** Fix-round Task 7 is on `main` (it makes `ClearSceneReferences` return `bool` and abort `ResetPerProjectState`'s scene half).

**Files:**
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/DeviceResourceLifetime.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/DeviceResourceLifetime.cpp`
- Modify: `BindlessTable.hpp` and `BindlessTable.cpp`
- Modify: `NriGraphContext.cpp` `ResolveMeshAlbedoSlot` (the Guid-only memo; re-read the function before editing) and `InvalidateMeshAlbedoSlot` (`NriGraphContext.hpp` near line 1578, a plain erase)
- Modify: the four caller sites listed in Step 3
- Test: `Tests/ArcaneTests/src/BindlessLifetimeTest.cpp` (new)

**Interfaces:**
- Consumes: `kBindlessCapacity` from Task 1. `kInvalidSlot` (`BindlessTable.hpp:55`). `kMaxFramesInFlight` is 3 (`FramePacing.hpp:39`).
- Produces: one `BindlessTable` per `NriDevice`. The device owns the descriptor-array allocation. `Add` pops a free index or appends. A live slot does not move. Add-only traffic still receives `0..n-1`. The map key is `(Guid, ColorSpace)`, not `Guid`.

`MeshNode::CreateBindless` (`MeshNode.cpp:288`) calls `BindlessTable::Create` per node today, and `AddMaterial` (`MeshNode.cpp:645-650`) calls `UpdateDescriptorRanges` only on that node's `m_bindlessSet`. Sharing the integer across contexts leaves the other context's set unwritten. When a slot is written, every context on that device that owns a bindless set gets `UpdateDescriptorRanges` at that index. The index is one device-wide value. The sets are updated together. Recycling still waits on `AllRetired` across every sampling context. A test samples the same slot index from two contexts and checks both contexts' descriptor sets contain the descriptor at that index.

```cpp
namespace Arcane
{
    struct LastUse
    {
        const void* context = nullptr;   // the NriGraphContext identity, not a fence from another timeline
        std::uint64_t fence = 0;
    };

    class ARC_RENDER_API DeviceResourceLifetime
    {
    public:
        void NoteSample(std::uint64_t slotKey, const void* context, std::uint64_t fence);
        // True when every NoteSample for slotKey has a completed fence.
        // A context with no note is not a waiter.
        [[nodiscard]] bool AllRetired(std::uint64_t slotKey, bool (*completed)(const void* context, std::uint64_t fence, void* user), void* user) const;
        void Clear(std::uint64_t slotKey);
    };
}
```

`Release(Guid, ColorSpace)`:

- Drops one material ref. The device table refcounts live materials per key. `Release` runs when the count hits zero. A material that never resolved a texture does not.
- Erases the mapping.
- Sets every resolved material and every GPU row whose `materialSlot` equals that index back to `kInvalidSlot` and marks those rows dirty, so the next `GpuSceneSync` rewrites them (`GpuSceneSync.hpp` copies `materialSlot` and rewrites when the resolved slot differs from `lastSlot`; re-read the 243–259 region and keep that compare).
- Does **not** destroy the descriptor in the caller, and does **not** return the index to the free list.
- Destruction runs when `AllRetired` is true: immediately if every recorded fence has completed, otherwise buried in the context that observed the completion, at **that** context's fence. Not `NriDevice::Graves()`. Not another context's fence value. The index returns to the free list at destroy.

`DeviceWaitIdle` inside `AddMaterial` goes away on the update-after-set path. On the fallback path the idle happens at most once per frame that rewrites a descriptor, not once per slot. A counter on the mock device covers both.

Callers, all four:

1. **Material destruction.** `SceneRenderResolver::InvalidateMaterial` and `EditorApp::InvalidateAssetCaches` for `AssetKind::Material` drop the ref. `EditorApp::InvalidateAssetCaches` is `EditorAppAssetOps.cpp:201-212`.
2. **Texture invalidation.** Cook-completion and asset-delete sites that today call `InvalidateContentTexture` and `InvalidateMeshAlbedoSlot` call `Release` for **both** `ColorSpace` values of that `Guid`. `NriTextureCache::Invalidate` already sweeps both spaces (re-read `NriTextureCache.hpp` around 230–233, including the chrome Display entry the editor drops near `EditorAppProject.cpp:833`).
3. **Project switch.** `EditorApp::ResetPerProjectState` already calls `SceneRenderResolver::Clear` (`EditorAppProject.cpp:2205-2215` on `a12f33e6`, `2191-2201` at fix `751786bd`; re-read after fix Task 7, and keep the resolver release on the path that runs when `ClearSceneReferences` aborts). It also `Release`s every mapping and dirties rows in that call.
4. **Eviction.** Task 4's texture eviction calls this `Release` before the texture object is destroyed. In this task, expose `Release` and call it from a test double. A mesh eviction frees no bindless slot. The test for caller 4 lives in this task as a direct `Release` of a texture key; Task 4's eviction path calls the same function.

`m_meshAlbedoSlots` leaves the context and sits on the device table.

- [ ] **Step 1: Write the failing device-less tests**

```cpp
TEST_CASE("a full table recycles only after every sampling fence retires", "[nri][bindless]")
{
    DeviceResourceLifetime life;
    BindlessTable& table = DeviceBindless();
    std::vector<std::uint32_t> first;
    for (std::uint32_t i = 0; i < Arcane::kBindlessCapacity; ++i)
        first.push_back(table.Add(MockSrv(i)));
    for (std::uint32_t slot : first)
        table.Release(slot);
    CompleteEveryFence(life);
    std::vector<std::uint32_t> refilled;
    for (std::uint32_t i = 0; i < Arcane::kBindlessCapacity; ++i)
    {
        const std::uint32_t slot = table.Add(MockSrv(i));
        CHECK(slot < Arcane::kBindlessCapacity);
        refilled.push_back(slot);
    }
    // The table is full. Add returns kInvalidSlot and consumes nothing
    // (BindlessTable.cpp:60-69). Calling Add for `held` here would make the
    // later CHECK compare two invalid slots and would not observe recycling.
    CHECK(table.Add(MockSrv(0)) == BindlessTable::kInvalidSlot);
    // Free one refilled slot. It has no NoteSample, and a context with no
    // note is not a waiter, so AllRetired is already true and Release
    // returns that index to the free list. The in-flight slot stays out.
    const std::uint32_t spare = refilled[0];
    table.Release(spare);
    const std::uint32_t held = refilled[1];
    life.NoteSample(held, ContextA(), 1);
    table.Release(held);
    const std::uint32_t next = table.Add(MockSrv(1));
    CHECK(next != BindlessTable::kInvalidSlot);
    CHECK(next == spare);
    CHECK(next != held);
}

TEST_CASE("context B does not recycle a slot context A is sampling", "[nri][bindless]")
{
    DeviceResourceLifetime life;
    const std::uint32_t s = DeviceBindless().Add(MockSrv(0));
    WriteBothSets(s); // both contexts' descriptor sets contain the descriptor at s
    CHECK(SetContains(ContextA(), s));
    CHECK(SetContains(ContextB(), s));
    life.NoteSample(s, ContextA(), 1); // A stays in flight
    DeviceBindless().Release(s);
    const std::uint32_t b = DeviceBindless().Add(MockSrv(1));
    CHECK(b != s);
    CompleteFence(ContextA(), 1);
    const std::uint32_t later = DeviceBindless().Add(MockSrv(2));
    CHECK(later == s);
}

TEST_CASE("add-only slots are 0..n-1", "[nri][bindless]")
{
    BindlessTable& table = DeviceBindless();
    CHECK(table.Add(MockSrv(0)) == 0u);
    CHECK(table.Add(MockSrv(1)) == 1u);
    CHECK(table.Add(MockSrv(2)) == 2u);
    CHECK(table.Add(MockSrv(3)) == 3u);
}

TEST_CASE("each release caller drops the mapping and dirties the row", "[nri][bindless]")
{
    for (int caller = 0; caller < 4; ++caller)
    {
        const std::uint32_t slot = ResolveAndNote(caller); // the four callers in the Interfaces list
        ReleaseCaller(caller);
        CHECK_FALSE(MappingExists(caller));
        CHECK(RowSlot(caller) == BindlessTable::kInvalidSlot);
        CHECK(RowDirty(caller));
        CHECK(NextAddWhileOtherContextInFlight() != slot);
        CompleteOtherContext();
        CHECK(DeviceBindless().Add(MockSrv(caller)) == slot);
    }
}
```

`DeviceBindless`, `MockSrv`, `WriteBothSets`, `SetContains`, and the caller helpers are test-local. They call `DeviceResourceLifetime` and a per-key `Release`. Today's `BindlessTable::Release` (`BindlessTable.hpp:87-93`) releases the whole table, not one slot, so these calls do not compile against it. The bodies are executable `CHECK`s. A comment-only body is not this test.

Plus a `[gpu]` case, name `"two contexts recycle a bindless slot without validation errors"`, tagged `[gpu]`. It runs the two-context shape on D3D12 and on Vulkan. `CHECK(Arcane::RenderErrorCount() == 0)`.

Idle counter case: a mock device that cannot update-after-set counts `DeviceWaitIdle`. Filling four slots in one frame that rewrites descriptors calls it once, not four times. The update-after-set mock counts zero inside `Add`.

- [ ] **Step 2: Build, then stop on the compile error**

Desk build. The cases call `DeviceResourceLifetime` and a per-key `Release`, which do not exist. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `DeviceResourceLifetime` or that `Release` as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red. The old whole-table `Release` (`BindlessTable.hpp:87-93`) is not what this run executes, because the new test does not compile against it.

- [ ] **Step 3: Implement the table, the record, and the four callers**

Rewrite the slot-policy comment. `Add` pops or appends. Per-key `Release` does not destroy. Destroy pushes the index. Move the memo off the context. Wire the four callers. Re-read each call site in the editor before editing it; the line hints in the spec (`EditorAppProject.cpp:828-843` and `:2214-2215`) were right in substance at the 2026-10-17 read and must be re-found if the editor moved.

Do not call `DeviceWaitIdle` per slot on the update-after-set path.

- [ ] **Step 4: Device-less tests, GPU test, golden, Servitor**

```powershell
.\ArcaneTests.exe "a full table recycles only after every sampling fence retires" --rng-seed 1
.\ArcaneTests.exe "context B does not recycle a slot context A is sampling" --rng-seed 1
.\ArcaneTests.exe "add-only slots are 0..n-1" --rng-seed 1
.\ArcaneTests.exe "each release caller drops the mapping and dirties the row" --rng-seed 1
.\ArcaneTests.exe "two contexts recycle a bindless slot without validation errors" --rng-seed 1
```

Expected: PASS, including `RenderErrorCount() == 0` on both backends. Skip a backend the machine does not have via `ARC_REQUIRE_BACKEND`; do not skip both. `run-tests.ps1 Debug` exits 0.

Golden gate, script path root-qualified:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1
```

Expected: 14/14, `diffCount` 0. No scene in the gate frees a material mid-frame, so indices stay. No `--bless`.

Servitor:

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-c --host runtime wait settled
& $S --project ReferenceProject --client plan-c --host runtime compare runtime-scene --source window
& $S --project ReferenceProject --client plan-c --host runtime quit
```

Expected: exit 0 on each, including `compare`. Exit 8 means pixels moved. Stop. Do not bless.

- [ ] **Step 5: Commit**

```
git add Source/Client/ArcaneRender Tests/ArcaneTests/src/BindlessLifetimeTest.cpp Source/Programs/ArcaneEditor
git commit -m "feat: recycle bindless slots after every sampling context retires"
```

with the trailer. Stage the editor files you actually changed.

---

### Task 3: Measure resident bytes, then set the provisional defaults

**Files:**
- Create: `Tests/ArcaneTests/src/ResidencyMeasureTest.cpp`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/RenderBudgetSettings.hpp`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/RenderBudgetSettings.cpp`
- The measurement log is not a committed binary. The commit message records the numbers.

**Interfaces:**
- Consumes: `MeshResidencyBytes` (`MeshResidencyBudget.hpp:76-82`). The existing mesh cvar, unchanged unless the predicate below fires.
- Produces: `RenderTextureSettings` and `RenderResidencySettings` in `RenderBudgetSettings.hpp`, registered with `ARC_SETTINGS` in `RenderBudgetSettings.cpp` beside line 7.

This task runs **before** the account exists. It does not evict anything.

Measurement, on this machine, written into the commit message:

- For each golden-gate scene the gate already loads, sum resident texture bytes (the bytes `NriTextureCache` would upload: mip footprints at the device's row and slice alignment, or, if no device is up, the source RGBA8/BC7 byte size the cache stores) and `MeshResidencyBytes` of each resident mesh.
- If `D:\dev\starworks\Aphelyon\Content` exists, cook or load the largest scene that checkout can cook and add those two sums. If the checkout is absent, write "Aphelyon Content absent, defaults unchanged by content" in the commit message and keep the provisional numbers. Do not invent a scene.
- Disk file sizes are not the answer. The spec's 179-byte png, 2124-byte glb, 28076-byte png, and 265-byte `.arcmesh` are source sizes. Do not treat them as resident bytes.

Predicate, applied independently to the texture sum and to the mesh sum. Let `measured` be that sum. The reservation is `cap * percent / 100`. Provisional texture cap is `512ull << 20`, percent is 50, so the reservation is 256 MiB. The mesh cap is already `512ull << 20`.

- If `measured * 4` would exceed that slice's reservation, raise **that slice's cap** so the reservation is at least `4 * measured`, rounded up to a power-of-two mebibyte, and still inside `67108864 .. 17179869184`. Percent stays 50 unless a cap at the range ceiling still cannot hold `4 * measured`, in which case stop and report. Do not silently clamp.
- Otherwise keep 512 MiB and 50.

Replace a default only in this commit, and only when the predicate says so. The account in Task 4 reads the latched caps. It does not hardcode 512.

Settings, copied from the mesh field's shape:

```cpp
struct RenderTextureSettings
{
    std::uint64_t residencyBudgetBytes = 512ull << 20; // or the measured replacement
};

ARC_REFLECT_TYPE(RenderTextureSettings)
    ARC_REFLECT_TYPE_ATTR(Settings, "render.texture", SettingScope::Project, ApplyMode::Restart, Audience::Game)
    ARC_REFLECT_FIELD(RenderTextureSettings, residencyBudgetBytes)
        ARC_REFLECT_ATTR(Range, 67108864.0, 17179869184.0)
        ARC_REFLECT_ATTR(Tooltip, "Texture slice of the device residency account, in bytes, latched at account creation.")
ARC_END_REFLECT_TYPE()

struct RenderResidencySettings
{
    std::uint32_t reservationPercent = 50;
};

ARC_REFLECT_TYPE(RenderResidencySettings)
    ARC_REFLECT_TYPE_ATTR(Settings, "render.residency", SettingScope::Project, ApplyMode::Restart, Audience::Game)
    ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
    ARC_REFLECT_FIELD(RenderResidencySettings, reservationPercent)
        ARC_REFLECT_ATTR(Range, 0.0, 100.0)
        ARC_REFLECT_ATTR(Tooltip, "Percent of each slice's own cap that the other slice may not borrow.")
ARC_END_REFLECT_TYPE()
```

`ARC_SETTINGS(Arcane::RenderTextureSettings);` and `ARC_SETTINGS(Arcane::RenderResidencySettings);` in `RenderBudgetSettings.cpp` after line 7. Not `ARC_CONSTANT`. Empty tooltips are refused.

- [ ] **Step 1: Write the failing registration test**

```cpp
TEST_CASE("texture and residency settings are registered", "[cvar][residency]")
{
    CHECK(Arcane::CVarRegistry::Get().Find("render.texture.residencyBudgetBytes") != nullptr);
    CHECK(Arcane::CVarRegistry::Get().Find("render.residency.reservationPercent") != nullptr);
    CHECK(Arcane::Settings<Arcane::RenderTextureSettings>().residencyBudgetBytes >= (64ull << 20));
    CHECK(Arcane::Settings<Arcane::RenderTextureSettings>().residencyBudgetBytes <= (16ull << 30));
    CHECK(Arcane::Settings<Arcane::RenderResidencySettings>().reservationPercent <= 100u);
}
```

Use the registry lookup the existing settings tests use if `Find` is not the name. The assertion is that both cvars exist and the texture cap is inside the mesh field's range.

- [ ] **Step 2: Build, then stop on the compile error**

The test names `Arcane::RenderTextureSettings` and `Arcane::RenderResidencySettings`, which do not exist yet. Desk build. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `RenderTextureSettings` as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red. "The cvars are not registered" would be the assertion only after the types exist and before `ARC_SETTINGS`. This red stops on the compile error. After both the types and the `ARC_SETTINGS` lines exist, the `Find` checks pass.

- [ ] **Step 3: Measure, then add the structs**

Do the measurement before editing the initializers. Record the two sums in the commit message. Apply the predicate. If both stay at 512 and 50, the message says so and quotes the sums. Add the structs and the two `ARC_SETTINGS` lines.

- [ ] **Step 4: Build and test**

Desk build. Expected: `ExitCode` 0. The new test PASS.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0. No golden gate (no pixel change).

Servitor. The new cvars are visible on a running host. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. Record every exit code. Quit even if a later command fails.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-c --host runtime wait settled
& $S --project ReferenceProject --client plan-c --host runtime cvar explain render.texture.residencyBudgetBytes
& $S --project ReferenceProject --client plan-c --host runtime cvar explain render.residency.reservationPercent
& $S --project ReferenceProject --client plan-c --host runtime quit
```

Expected: every exit 0. Each `cvar explain` envelope names that cvar. A missing name is exit 6, not a pass.

Confirm `build/abi-digest.txt` is unchanged (`git diff build/abi-digest.txt` empty). These headers are not on the game-facing list.

- [ ] **Step 5: Commit**

```
git add Source/Client/ArcaneRender/src/Arcane/Render/RenderBudgetSettings.hpp Source/Client/ArcaneRender/src/Arcane/Render/RenderBudgetSettings.cpp Tests/ArcaneTests/src/ResidencyMeasureTest.cpp
git commit -m "feat: register the texture residency cap and the reservation percent"
```

The body of the message contains the two measured sums, whether Aphelyon Content was present, and which defaults moved. Trailer as in Global Constraints.

---

### Task 4: One residency account

**Files:**
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/ResidencyAccount.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/ResidencyAccount.cpp`
- Modify: `NriMeshBufferCache` and `NriTextureCache` so both vehicles on one device charge the same account. `NriGraphContext.cpp` creates both caches (the create is near lines 441–445 on `a12f33e6`, 442–446 after fix Task 2; re-read it). They stop latching a private 512.
- Modify: `MeshResidencyBudget.hpp:32-39` so the comment says the budget is the device account, not per vehicle.
- Test: `Tests/ArcaneTests/src/ResidencyAccountTest.cpp` (new)

**Interfaces:**
- Consumes: the two settings from Task 3, latched at account creation the way `NriMeshBufferCache.cpp:32` latches today. `DeviceResourceLifetime` from Task 2. An evicted GPU object waits on that record. `Graves()` is not the clock.
- Produces: the functions below. Admission is device-less and does not touch NRI.

```cpp
namespace Arcane
{
    enum class ResidencySlice : std::uint8_t { Mesh, Texture };

    struct ResidencyEntry
    {
        Guid id{};
        ResidencySlice slice = ResidencySlice::Mesh;
        // Texture identity includes ColorSpace. Mesh entries leave this at Srgb
        // and do not use it. NriTextureCache::Key is (Guid, ColorSpace)
        // (NriTextureCache.hpp:323-331). ColorSpace is Srgb or Display (line 117).
        // One Guid resident in both spaces is two entries.
        NriTextureCache::ColorSpace space = NriTextureCache::ColorSpace::Srgb;
        std::uint64_t bytes = 0;
        std::uint64_t lastDrawnFrame = 0;
    };

    struct ResidencyEviction
    {
        Guid id{};
        ResidencySlice slice = ResidencySlice::Mesh;
        NriTextureCache::ColorSpace space = NriTextureCache::ColorSpace::Srgb;
    };

    struct ResidencyCaps
    {
        std::uint64_t meshCap = 0;
        std::uint64_t textureCap = 0;
        std::uint32_t reservationPercent = 50;
    };

    [[nodiscard]] inline std::uint64_t Reservation(std::uint64_t cap, std::uint32_t percent) noexcept
    {
        return cap * static_cast<std::uint64_t>(percent) / 100ull; // multiply, then divide
    }

    // 0 when used >= cap; cap - reserve when used <= reserve; cap - used otherwise.
    [[nodiscard]] std::uint64_t UnusedSurplus(std::uint64_t used, std::uint64_t cap, std::uint64_t reserve) noexcept;

    // Charges `incoming` onto `requester` first, with lastDrawnFrame = currentFrame.
    // Evicts whole entries until overSlice(mesh), overSlice(tex), and overTotal are all false.
    // Returns the evicted entries in order, each with Guid, slice, and ColorSpace.
    // The incoming entry is never a victim
    // (its lastDrawnFrame is currentFrame).
    [[nodiscard]] std::vector<ResidencyEviction> Admit(
        std::vector<ResidencyEntry>& entries,
        ResidencyEntry incoming,
        ResidencySlice requester,
        const ResidencyCaps& caps,
        std::uint64_t currentFrame);
}
```

`overSlice(slice)` is `used(slice) > allowance(slice)`. Allowance is own cap plus the other slice's `UnusedSurplus`. `overTotal` is `used(mesh) + used(tex) > meshCap + textureCap`.

Stop **only** when all three predicates are false. Fitting the requester's own cap is not a stop.

Evict one entry at a time and recompute surplus after each:

1. While `overSlice(requester)`, the victim is the requester's oldest cold entry, smaller `Guid` on a tie. The reservation does not shield the requester from its own charge.
2. Otherwise the victim is the oldest cold entry on a slice whose `used` is still above its reservation. Equal `lastDrawnFrame`: a texture before a mesh. Equal frame and the same slice: smaller `Guid`. A slice at or under its reservation contributes no victim in this arm.

An entry with `lastDrawnFrame == currentFrame` is never a victim, on either slice, whichever vehicle asked. When the loop ends with a predicate still true, every eligible cold entry is already gone. The frame renders. The once-per-report log stays.

Locked case, at whatever caps Task 3 left. The test builds the caps as 512 MiB and 50 percent even if Task 3 raised a default, by passing `ResidencyCaps` explicitly. Production uses the latched settings. The test's locked numbers are the spec's provisional case, not a second production default:

- T1 texture, 188 MiB, `lastDrawnFrame` 1, guid `Guid{1, 0}`
- T2 texture, 512 MiB, `lastDrawnFrame` 2, guid `Guid{2, 0}`
- Meshes empty. `currentFrame` 10.
- Admit mesh M, 512 MiB, `lastDrawnFrame` set by `Admit` to 10, guid `Guid{3, 0}`.

Rule 1 does not fire (`512 <= 512` against the mesh cap, so the mesh slice is not over its allowance before borrowing is recomputed — after the charge, mesh used is 512, mesh allowance is 512 + texture surplus; texture used 700 is over the new texture allowance of 512 because mesh surplus dropped to 0). The result after eviction: mesh used 512, texture used 512, sum 1024, T1 is the only eviction, T2 and M stay. A result whose sum is 1212 fails.

188 MiB is `188ull << 20`. 512 MiB is `512ull << 20`.

Two caches, one account: the test builds two mesh-cache facades over one `Admit` list, charges one mesh into the first, and the second charge of another mesh that does not fit the shared cap evicts from the shared set. It must not succeed inside a private second 512.

Drawn-this-frame: a mesh with `lastDrawnFrame == currentFrame` is absent from the eviction list even when the requesting cache is a different vehicle.

Two-context destroy: reuse Task 2's record. Context A has sampled a mesh or a texture and stays in flight. Context B's eviction does not destroy that object and does not reuse its memory. After A's fence completes, the destroy runs.

- [ ] **Step 1: Write the failing admission tests**

Names:

- `"700 MiB of textures plus a 512 MiB mesh sums to 1024"`
- `"a second vehicle charges the same account"`
- `"a texture admit borrows mesh surplus and does not evict a mesh inside the reservation"`
- `"equal age evicts the texture before the mesh"`
- `"a mesh drawn this frame is not evicted for another vehicle"`
- `"an evicted object waits on the sampling context"`
- `"one guid in Srgb and Display is two entries"`

The two-space case admits the same `Guid` twice, once with `ColorSpace::Srgb` and once with `ColorSpace::Display`. The account holds two entries. Evicting the Srgb entry leaves the Display entry. Mesh entries carry no meaningful color space. The locked 700 MiB case keeps its distinct guids.

The 700-case test contains:

```cpp
CHECK(sum == (1024ull << 20));
CHECK(meshUsed == (512ull << 20));
CHECK(textureUsed == (512ull << 20));
CHECK(evicted.size() == 1);
CHECK(evicted[0].id == Arcane::Guid{1, 0}); // T1
CHECK(evicted[0].slice == Arcane::ResidencySlice::Texture);
```

- [ ] **Step 2: Build, then stop on the compile error**

Desk build. The cases call `Admit`, which does not exist. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `Admit` as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red.

- [ ] **Step 3: Implement `Admit` and point both caches at one account per device**

`NriMeshBufferCache::Create` and the texture cache's create take the device account (or create it on the device if this is the first cache). They do not each store a private `m_budget` of 512 that `SelectEvictions` uses alone. `SelectEvictions` can stay for tests that pass an explicit budget; the caches call `Admit`. Rewrite the per-vehicle comment at `MeshResidencyBudget.hpp:32-39`.

`16 GiB * 100` fits in `uint64`. The reservation multiply is `cap * percent / 100`, never `cap / 100 * percent` (the latter truncates).

- [ ] **Step 4: Tests and suite**

Desk build. Expected: `ExitCode` 0. All seven cases PASS at `--rng-seed 1`, including the two-space case.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0. No golden gate yet (Task 5 changes the upload path; the account at these caps does not evict the gated scenes, and Task 5's gate covers the pixels). If Task 3 raised a cap, say so in this commit message. The locked test still passes 512/50 because it passes caps explicitly.

Servitor. The caches change what a host keeps resident at boot. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. Record every exit code. Quit even if a later command fails.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-c --host runtime wait settled
& $S --project ReferenceProject --client plan-c --host runtime cvar explain render.texture.residencyBudgetBytes
& $S --project ReferenceProject --client plan-c --host runtime quit
```

Expected: every exit 0. `wait settled` exit 0 is the host booting with the account in place. The cvar explain exit 0 shows the latched cap is the registered setting. A dead host is exit 7.

- [ ] **Step 5: Commit**

```
git add Source/Client/ArcaneRender/src/Arcane/Render/Nri/ResidencyAccount.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/ResidencyAccount.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriMeshBufferCache.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriTextureCache.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/MeshResidencyBudget.hpp Tests/ArcaneTests/src/ResidencyAccountTest.cpp
git commit -m "feat: charge meshes and textures to one residency account per device"
```

with the trailer.

---

### Task 5: Upload node, footprint, and ring fit

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriUploadRing.hpp` and `NriUploadRing.cpp`
- Modify: `RenderGraph.hpp:149-154` (add two enumerators)
- Modify: `RenderGraph.cpp` `StateFor` (a case for each)
- Modify: `RenderGraphExec.cpp` `TextureUsageFor` (both new usages fall through like `IndirectArgs`)
- Modify: `NriTextureCache.cpp` (lines 339, 454, 582) and `NriMeshBufferCache.cpp` (line 221)
- Modify: the mesh and pick setup lambdas so the upload frame declares `VertexBuffer` and `IndexBuffer` reads. The setup sites are `AddMeshNode` (`MeshNode.cpp:1241`, the `"mesh"` node's builder lambda) and `AddPickNodes` (`PickOutlineNodes.cpp:1270`, `graph.AddNode("pick"` at line 1289; the comment at 1266 says "The declarators"). `MeshNode.cpp:1143-1145` and `PickOutlineNodes.cpp:611-612` are `CmdSetVertexBuffers` / `CmdSetIndexBuffer` inside Record, after `Compile`. Those stay the record calls. They are not where the reads are declared.
- Test: `Tests/ArcaneTests/src/UploadStagingTest.cpp` (new)

**Interfaces:**
- Consumes: `RingLayout::AlignUp` (`NriUploadRing.cpp:30-35`) and `Allocate` (`:55-76`). `StateFor`'s `CopyDst` row (`RenderGraph.cpp:109-111`) and the buffer layout force (`:125-130`). `kShaderReadStages` (`:70-71`).
- Produces: `RingLayout::Fits(uint64_t size, uint64_t align) const`, used by `Allocate`. `RgUsage::VertexBuffer` and `RgUsage::IndexBuffer`. A copy node named `upload`. `RenderGraphNodeContext::SuppressConsumers(RgTexture)` and `SuppressConsumers(RgBuffer)`: each decodes the handle and inserts that `(resourceIndex, isTexture)` pair. `RenderGraph::DebugSkippedBarrierCount() const` beside `DebugSubmitCount` (`RenderGraph.hpp:769`), a `std::uint64_t` cleared when `Execute` clears the suppress set and incremented once per barrier `EmitBarriers` drops.

`Fits` is the overflow condition `Allocate` already uses, negated:

```cpp
bool RingLayout::Fits(std::uint64_t size, std::uint64_t align) const
{
    const std::uint64_t alignedOffset = AlignUp(m_cursor, align);
    if (alignedOffset > m_capacity) return false;
    if (size > m_capacity - alignedOffset) return false;
    return true;
}
```

`Allocate` calls `Fits`. On false it increments `m_overflowCount` and returns `{0, false}`, as it does today. `Fits` does **not** increment the counter. The planner calls `Fits`. An oversized payload that never calls `Allocate` does not bump the counter. `NriUploadRing::Fits` forwards to the current slot's `RingLayout` and does not allocate. The GPU exec calls that after `BeginFrame`. A NONE-device test does not call it: `NriUploadRing::Init` fails on NONE because `MapBuffer` returns null (`NriUploadRing.hpp:25-30`), so that test drives a standalone `RingLayout`, which is the math `BeginFrame` resets.

`ring.BeginFrame(slot)` runs after `BuildFrame` returns and after `Compile` (`NriGraphContext.cpp:1304-1308` says a setup-fn allocation lands in the previous slot). `RenderFrame` calls `BuildFrame` at 1962, `Compile` at 1968, `BeginFrame` at 1981. `RenderFrameOffscreen` waits the pacing fence, then `BuildFrame` at 2111, `Compile` at 2117, `BeginFrame` at 2125. `RingLayout::Init` sets the cursor to 0 (`NriUploadRing.cpp:42-47`). `Reset` zeros it (lines 50–53). `Allocate` uses the subtraction form (lines 55–76).

Reservation, both the window path and the offscreen path. The setup lambdas inside `BuildFrame` (the `AddNode` builders, before `Compile`) only declare the upload node, the `CopyDst` writes, and the consumer reads. They do not call `Fits` or `Allocate`. The exec function runs after `BeginFrame` in both `RenderFrame` and `RenderFrameOffscreen`. That is where the payload is classified and, when it takes the ring, allocated on that frame's cursor. A test drives both paths.

Classification, `capacity` = the latched `uploadRingBytesPerFrame`, evaluated in the exec function:

- `Fits(alignedBytes, alignment)` is true: take the bytes from the recording context's ring. The resident object stays `MemoryLocation::DEVICE`.
- `alignedBytes <= capacity` and `Fits` is false: bury the device-local object on the recording context's lane at that context's current submit value, or fence 0 if this frame never submitted, and retry next frame. `BeginFrame` / `Reset` sets the cursor to 0 (`NriUploadRing.cpp:50-53`), so a payload that fits an empty slot can succeed. The entry is not marked ready. Do not call `Allocate` for this payload, so the overflow count does not move. Classification stays in this exec function, after `BeginFrame`. Do not move it back into the setup lambda.

The executor has no skip. It emits every compiled node's `preBarriers`, then calls every node's exec, then emits `compiled.exitBarriers` (`RenderGraphExec.cpp:1353-1424`). `EmitBarriers` is the only `CmdBarrier` site (`RenderGraphExec.cpp:11-14` and `:1084`). A consumer read declared at setup is already a barrier in `RgCompiledNode::preBarriers` (`RenderGraph.hpp:443`) by the time the upload exec decides to retry. "Nothing samples it" is not a mechanism: the mesh node still runs, and its barrier still transitions the resource.

The upload exec, on this retry branch only, calls `RenderGraphNodeContext::SuppressConsumers` for the texture and for both buffers. The context's `graph` pointer stays const (`RenderGraph.hpp:250`). `RenderGraph` is already a friend (`:844`) and owns a mutable set of decoded `(resourceIndex, isTexture)` pairs, the same pair `RgBarrier` stores (`:434-435`). `Execute` clears that set at the start of the record loop, before the first node. The upload node's own pre-barriers have already been emitted (`:1362` runs before exec at `:1418`); the entry transition to `CopyDst` stays. Every later `EmitBarriers` call, including exit barriers, drops a barrier whose pair is in the set: it is not passed to `CmdBarrier`, and it does not update that slot's pool carry (`:1064-1073` does not run for a dropped barrier). The consumer exec still runs, so other draws in that node are kept. It does not bind, `CmdSetVertexBuffers`, `CmdSetIndexBuffer`, or sample a resource whose entry is not ready. The buried object is referenced in this command buffer only by the upload entry barrier. Destruction waits for this submit's fence. Next frame imports a new object at `kUnknownState`. The ring-hit branch does not call `SuppressConsumers`, so the same-frame consumer barriers and the sample stay.
- `alignedBytes > capacity`: do not retry and do not call `Allocate`. Create one committed buffer at `MemoryLocation::HOST_UPLOAD` sized to `alignedBytes`. `BufferUsageBits::NONE` is legal. Map, write, copy this frame. The staging buffer is not a graph resource. Bury **the staging buffer**, not the device-local resource, in the recording context's graveyard at that submit's fence. The device-local resource is ready this frame.

"Fits an empty slot" is `alignedBytes <= capacity` (cursor 0 is already aligned).

Mesh `alignedBytes`: vertex region and index region each rounded up with `AlignUp(..., 16)`, then summed. The ring call is `Allocate(alignedBytes, 16)` when it fits.

Texture `alignedBytes`: for each mip, `sliceRowNum = slicePitch / rowPitch`, `alignedRowPitch = Align(rowPitch, rowAlign)`, `alignedSlicePitch = Align(sliceRowNum * alignedRowPitch, sliceAlign)`, footprint `alignedSlicePitch * sliceNum`, summed. `Align` here is the bitmask `(x + alignment - 1) & ~(alignment - 1)` because that is what NRI's helper uses (`SharedExternal.h`; re-read `HelperInterface.hpp` around 305–308 and 375 of `SharedExternal.h` before writing the function, and match it). For the power-of-two alignments this test injects, the bitmask and `AlignUp` agree. Use the bitmask for the footprint and `AlignUp` for the ring. Source pitches stay what the cache writes today: BC7 uses `Bc7RowPitch` / `Bc7SlicePitch` (`NriTextureCache.cpp:540-558`), RGBA8 uses `width * 4` and `width * height * 4` (`:563` and the content path at `:330-331`). Do not use `Create`'s `(alignedSize * 4) / 3` mip-0 guess.

CPU write, on the ring and on the dedicated buffer: for slice `k` and row `l` in `0 .. sliceRowNum - 1`, copy `rowPitch` bytes from `src + k * slicePitch + l * rowPitch` to `dst + k * alignedSlicePitch + l * alignedRowPitch`. `CmdUploadBufferToTexture` gets `rowPitch = alignedRowPitch`, `slicePitch = alignedSlicePitch`, `offset` = the aligned buffer position. The next mip begins `alignedSlicePitch * sliceNum` bytes later.

The upload node is named `upload`, declared at the start of that context's frame, before Batch2D, the id pass, Mesh, and any sampler. Its exec fn calls `CmdCopyBuffer` for the vertex buffer and the index buffer, and one `CmdUploadBufferToTexture` per mip. It does not emit a barrier. The executor remains the only `CmdBarrier` site.

Declared write: `RgUsage::CopyDst` on the texture and on both buffers.

Consumer reads, declared in the setup lambdas named above, before `Compile()`, on the upload frame:

- Texture: existing `RgUsage::ShaderRead` (not the helper's fragment-only after-state at `NriTextureCache.cpp:336-337` and `:451-452` and `:579-580`). `builder.Read` of the texture stays where that read is already declared.
- Vertex buffer: `builder.Read(vertexBuffer, RgUsage::VertexBuffer)` inside `AddMeshNode`'s builder and inside `AddPickNodes`' `"pick"` builder. `StateFor` returns access `VERTEX_BUFFER`, layout `UNDEFINED`, stage `VERTEX_SHADER`.
- Index buffer: `builder.Read(indexBuffer, RgUsage::IndexBuffer)` in those same two builders. `StateFor` returns access `INDEX_BUFFER`, layout `UNDEFINED`, stage `INDEX_INPUT`.

Record stays the `CmdSetVertexBuffers` / `CmdSetIndexBuffer` calls. Do not move those calls into the setup lambda.

`TextureUsageFor` returns no texture bit for either new usage, in the same `break` as `IndirectArgs` (`RenderGraphExec.cpp:224`).

On the upload frame the node imports the resources. A new texture's import starts at `kUnknownState`, so the first barrier enters `CopyDst` and the exit is the `ShaderRead` triple. `ImportBuffer` takes no entry and no exit (`RenderGraph.hpp:169`). The first barrier leaves `kUnknownState` for `CopyDst`. Later frames do not re-import.

Failure: `CreateCommittedBuffer` / `CreateCommittedTexture` failure leaves the entry not ready and memoized refused (today's abandon). Dedicated staging create or map failure is the same memoized refusal. `Execute` failure before submit uses the fence-0 burial. Another context samples only after a queue wait on the uploading context's pacing fence, recorded in `DeviceResourceLifetime`. Not `DeviceWaitIdle`. No new copy queue.

Pinned footprint results, row alignment 256, slice alignment 512. Source pitches in parentheses:

| Mip | Result |
|---|---|
| RGBA8 1×1 (row 4, slice 4) | 512 |
| RGBA8 5×5 (row 20, slice 100) | 1536 |
| BC7 5×5 (32, 64) | 512 |
| BC7 2×2 (16, 16) | 512 |
| BC7 1×1 (16, 16) | 512 |
| BC7 chain 5→2→1 | 1536 |
| RGBA8 chain 5×5, 2×2 (row 8, slice 16), 1×1 | 2560 |

A 1×1 result of 16, a missed mip, or the mip-0 guess, fails.

Non-aligned capacity pin: capacity `(4ull << 20) + 511`, cursor `(4ull << 20) - 16`, alignment 512, `alignedBytes` 512. `Fits` is false. `Allocate` is false and its overflow count increased by exactly 1. `Fits` itself does not change the count; call it first, then `Allocate`, and check the count moved only on `Allocate`.

Oversized, capacity `4ull << 20`: mesh vertex region = capacity, index region = 16, so `alignedBytes = capacity + 16`, class dedicated. Texture mip footprints sum to `capacity + 512` under alignments 256 and 512, class dedicated. Neither is retry. Neither increments the overflow count.

`[gpu]` first-use: resolve a fresh texture and a fresh mesh during a frame that already has other work. Read back. Compare to a picture captured from today's waiting `UploadData` path for the same bytes. `diffCount == 0` on D3D12 and Vulkan, `RenderErrorCount() == 0`, and the `DeviceWaitIdle` count on that path is 0. Two-context: B's first sample of a resource A uploaded, while A is still in flight, matches after B's queue wait on A's fence, idle count still 0. Oversized texture and oversized mesh (staging bytes greater than the latched ring) are ready the same frame, match their picture, `RenderErrorCount() == 0`, and do not increment the ring overflow count for that payload.

- [ ] **Step 1: Write the failing device-less tests**

```cpp
TEST_CASE("texture footprint matches the helper at 256 and 512", "[nri][upload]")
{
    CHECK(FootprintRgba8(1, 1, 256, 512) == 512u);
    CHECK(FootprintRgba8(5, 5, 256, 512) == 1536u);
    CHECK(FootprintBc7(5, 5, 256, 512) == 512u);
    CHECK(FootprintBc7(2, 2, 256, 512) == 512u);
    CHECK(FootprintBc7(1, 1, 256, 512) == 512u);
    CHECK(FootprintBc7Chain(256, 512) == 1536u);
    CHECK(FootprintRgba8Chain(256, 512) == 2560u);
}

TEST_CASE("ring fit agrees with Allocate on a non-aligned capacity", "[nri][upload]")
{
    RingLayout ring;
    ring.Init((4ull << 20) + 511ull); // Init sets the cursor to 0
    // A legal prefix. AlignUp(0, 1) + size <= capacity, so this succeeds and
    // leaves the cursor at 4MiB - 16. Do not add a setter for m_cursor.
    const auto prefix = ring.Allocate((4ull << 20) - 16ull, 1ull);
    REQUIRE(prefix.ok);
    CHECK(ring.OverflowCount() == 0u);
    const std::uint64_t before = ring.OverflowCount();
    CHECK_FALSE(ring.Fits(512, 512));
    CHECK(ring.OverflowCount() == before);          // Fits does not bump
    CHECK_FALSE(ring.Allocate(512, 512).ok);
    CHECK(ring.OverflowCount() == before + 1);      // Allocate bumps by exactly 1
}

TEST_CASE("capacity plus 16 and capacity plus 512 are dedicated", "[nri][upload]")
{
    const std::uint64_t capacity = 4ull << 20;
    CHECK(Classify(capacity, capacity + 16, 16) == Staging::Dedicated);
    CHECK(Classify(capacity, capacity + 512, 512) == Staging::Dedicated);
    CHECK(Classify(capacity, 1024, 16) == Staging::Ring);          // fits an empty slot
    // remaining short, payload fits an empty slot:
    CHECK(ClassifyAtCursor(capacity, capacity - 16, 4096, 16) == Staging::Retry);
}

TEST_CASE("a ring miss in a frame with a consumer emits no consumer barrier and does not sample", "[nri][upload]")
{
    // Tree: AddNode, Compile, Execute, CreateTexture, Write, Read,
    // RgUsage::CopyDst, RgUsage::ShaderRead, RgBarrier, RingLayout::Init,
    // Reset, Allocate, OverflowCount (NriUploadRing.hpp:76-104),
    // NriDevice::CreateNoneForTests, RgExecuteDesc, RenderErrorCount.
    // Task 5, not on the tree: RingLayout::Fits, SuppressConsumers(RgTexture),
    // DebugSkippedBarrierCount. Do not Init NriUploadRing: MapBuffer returns
    // null on NONE (NriUploadRing.hpp:25-30). A standalone RingLayout is what
    // BeginFrame resets. CreateTexture, not ImportTexture(nullptr): Execute
    // refuses a barrier whose resolved texture is null
    // (RenderGraphExec.cpp:1257-1284). The pool realizes a non-null dummy.
    const std::uint64_t errorsBefore = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    Arcane::RingLayout layout;
    layout.Init(4ull << 20);

    Arcane::RgTexture tex;
    Arcane::RgTextureDesc texDesc;
    texDesc.format = nri::Format::RGBA8_UNORM;
    texDesc.width = 1;
    texDesc.height = 1;

    bool miss = true;
    bool ready = true;
    bool prefixOk = false;
    bool payloadOk = false;
    bool suppressed = false;
    int meshExecs = 0;
    int samples = 0;

    graph.AddNode("upload", Arcane::RenderGraph::NodeKind::Copy,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("uploaded", texDesc);
            builder.Write(tex, Arcane::RgUsage::CopyDst);
        },
        [&](Arcane::RenderGraphNodeContext& context)
        {
            layout.Reset();
            prefixOk = true;
            if (miss)
                prefixOk = layout.Allocate((4ull << 20) - 16ull, 1ull).ok;
            constexpr std::uint64_t payload = 4096;
            if (payload <= layout.Capacity() && !layout.Fits(payload, 16ull))
            {
                context.SuppressConsumers(tex);
                suppressed = true;
                ready = false;
            }
            else
            {
                payloadOk = layout.Allocate(payload, 16ull).ok;
                ready = payloadOk;
            }
        });

    graph.AddNode("mesh", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.Read(tex, Arcane::RgUsage::ShaderRead);
        },
        [&](Arcane::RenderGraphNodeContext&)
        {
            ++meshExecs;
            if (ready)
                ++samples;
        });

    std::string error;
    const std::optional<Arcane::RgCompiled> compiled = graph.Compile(&error);
    INFO(error);
    REQUIRE(compiled.has_value());
    REQUIRE(compiled->nodes.size() == 2);
    REQUIRE(compiled->nodes[0].preBarriers.size() == 1);
    REQUIRE(compiled->nodes[1].preBarriers.size() == 1);
    CHECK(compiled->exitBarriers.empty());

    const Arcane::RgBarrier& entry = compiled->nodes[0].preBarriers[0];
    const Arcane::RgBarrier& consumer = compiled->nodes[1].preBarriers[0];
    CHECK(entry.isTexture);
    CHECK(entry.resourceIndex == 0u);
    CHECK(static_cast<std::uint32_t>(entry.after.access)
          == static_cast<std::uint32_t>(nri::AccessBits::COPY_DESTINATION));
    CHECK(consumer.isTexture);
    CHECK(consumer.resourceIndex == 0u);
    CHECK(static_cast<std::uint32_t>(consumer.before.access)
          == static_cast<std::uint32_t>(nri::AccessBits::COPY_DESTINATION));
    CHECK(static_cast<std::uint32_t>(consumer.after.access)
          == static_cast<std::uint32_t>(nri::AccessBits::SHADER_RESOURCE));
    CHECK(static_cast<std::uint32_t>(consumer.after.layout)
          == static_cast<std::uint32_t>(nri::Layout::SHADER_RESOURCE));
    const nri::StageBits shaderRead =
        nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;
    CHECK(static_cast<std::uint32_t>(consumer.after.stages)
          == static_cast<std::uint32_t>(shaderRead));

    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

    meshExecs = 0;
    samples = 0;
    REQUIRE(graph.Execute(desc, *compiled));
    CHECK(prefixOk);
    CHECK(suppressed);
    CHECK_FALSE(ready);
    CHECK(meshExecs == 1);
    CHECK(samples == 0);
    CHECK(graph.DebugSkippedBarrierCount() == 1u);
    CHECK(layout.OverflowCount() == 0u);
    CHECK(compiled->nodes[1].preBarriers.size() == 1);

    miss = false;
    prefixOk = false;
    payloadOk = false;
    suppressed = false;
    meshExecs = 0;
    samples = 0;
    REQUIRE(graph.Execute(desc, *compiled));
    CHECK(payloadOk);
    CHECK_FALSE(suppressed);
    CHECK(ready);
    CHECK(meshExecs == 1);
    CHECK(samples == 1);
    CHECK(graph.DebugSkippedBarrierCount() == 0u);
    CHECK(layout.OverflowCount() == 0u);
    CHECK(Arcane::RenderErrorCount() == errorsBefore);
}
```

`Classify` and `Footprint*` are the production functions, not copies in the test. A mapping-table test walks every `RgUsage` enumerator and checks `StateFor` does not return `kUnknownState`. `VertexBuffer` and `IndexBuffer` are in that walk. `TextureUsageFor` of both is `NONE`.

- [ ] **Step 2: Build, then stop on the compile error**

Desk build. The cases call `FootprintRgba8`, `RingLayout::Fits`, `SuppressConsumers`, and `DebugSkippedBarrierCount`, which do not exist. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names one of those as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red.

The ring-miss case's own red is the same compile error, not an assertion and not a passing comment. Its body calls `RingLayout::Fits` (`NriUploadRing.hpp:97` is `Allocate`; there is no `Fits`), `RenderGraphNodeContext::SuppressConsumers` (`RenderGraph.hpp:225-251` has `Resolve`, `ColorView`, and `graph` only), and `RenderGraph::DebugSkippedBarrierCount` (the next method after `DebugSubmitCount` at `RenderGraph.hpp:769` is `Graves()`). `logs\build-tests.log` names `SuppressConsumers` and `DebugSkippedBarrierCount`. The translation unit's first diagnostic may be `FootprintRgba8` from the case above; this case still does not compile, so there is no sample-count failure to observe. A comment-only body would have compiled and passed.

The `Fits(512, 512)` assertion is not what this run observes. After implementation, with the cursor still at 0, `Fits(512, 512)` on a `4 MiB + 511` ring would be true; the prefix allocate is what makes the assertion the one this plan states.

- [ ] **Step 3: Implement fit, footprint, usages, and the upload node**

Extract `Fits`. Add the two enumerators and both `StateFor` cases and the `TextureUsageFor` fall-through. Replace all four `UploadData` call sites. Declare the `upload` node. Bind the new reads on the upload frame only. Add `SuppressConsumers(RgTexture)` and `SuppressConsumers(RgBuffer)` on `RenderGraphNodeContext`, and `DebugSkippedBarrierCount()` beside `DebugSubmitCount` (`RenderGraph.hpp:769`). `Execute` zeros that counter where it clears the suppress set. `EmitBarriers` increments it once per barrier it drops. The miss frame therefore reads 1 (the mesh `ShaderRead` barrier only; the upload `CopyDst` barrier was already emitted). The empty-ring frame reads 0. The retry branch calls `SuppressConsumers`. The ring-hit branch does not.

The `[gpu]` picture test stores the `UploadData` reference by running the old helper once into an offscreen target **before** the new path is the only path, or by keeping a test-only helper call that is not the production path. Do not leave production on `UploadData`. The compare is byte equality of the readback (`diffCount == 0`), not a golden file in `ReferenceProject/Verify`.

- [ ] **Step 4: Device-less tests, GPU tests, golden, Servitor**

The device-less cases PASS, including the ring-miss frame whose compiled consumer barrier is skipped (`DebugSkippedBarrierCount() == 1`) and whose sample count stays 0, and the next frame, which samples once with that count at 0. The `[gpu]` case PASS on D3D12 and Vulkan: `diffCount == 0`, `RenderErrorCount() == 0`, idle count 0, oversized pair included, overflow count unchanged for that payload. `run-tests.ps1 Debug` exits 0.

Golden gate, script path root-qualified:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1
```

Expected: 14/14, `diffCount` 0. At the latched defaults the gated scenes sit inside the mesh reservation, so nothing is evicted. No `--bless`.

Servitor, same commands as Task 2 (`plan-c`, `compare runtime-scene --source window`). `launch` takes no `--client`. Record every exit code. Expected: every exit 0, including `compare`. Exit 8 is a pixel miss. Then quit.

- [ ] **Step 5: Commit**

```
git add Source/Client/ArcaneRender Tests/ArcaneTests/src/UploadStagingTest.cpp
git commit -m "feat: upload meshes and textures in a CopyDst node without waiting"
```

with the trailer.

---

### Task 6: Fix #11 completion check

**Files:**
- Create: `Tests/ArcaneTests/src/ProcessSchedulerCompleteTest.cpp`
- Do not modify `ProcessContext`, `JobSystem`, or `Runtime.cpp` in this task.

**Interfaces:**
- Consumes: `ProcessContext.hpp` after fix Task 12 (the accessor it adds). `Runtime.cpp:200` and `:239` still construct one `JobSystem` per world. `JobSystem.hpp:3-7` still claims one per process. Two `Runtime`s already share a `TypeContext` (`ProcessContextTest.cpp:72-77`). That is not the scheduler check.
- Produces: a Catch case named exactly `two Runtimes in one process share one scheduler`.

Look at `ProcessContext` before writing the test.

- If it still has no accessor that returns the process `JobSystem` or its `IWorkScheduler`: add the hidden test below. Do not add the accessor. Do not include a header that does not exist.

```cpp
TEST_CASE("two Runtimes in one process share one scheduler", "[.][arc-complete]")
{
    FAIL("fix #11 is not in this tree: ProcessContext does not expose one process JobSystem");
}
```

`[.]` hides it from `~[gpu]` and from `run-tests.ps1`. Invoking it by name runs it, and it fails. That is the record "merged, arc not complete."

- If a later merge already added the accessor: do not use `[.]` and do not `FAIL`. Construct two `Arcane::Runtime`s on `Arcane::Test::Process()`, and `CHECK` that the scheduler each world uses is the same pointer as `ProcessContext`'s accessor. Also `CHECK` that destroying one `Runtime` does not destroy that scheduler (the other `Runtime` still steps). The destructor order is whatever fix Task 11 left in `JobSystem.cpp`. This task does not change it. This task still does not implement that destructor. It only asserts the pointer.

- [ ] **Step 1: Write the test that matches the tree you have**

Use the hidden `FAIL` form unless the accessor is already there.

- [ ] **Step 2: Build, then run it by name**

The hidden case compiles: it calls `FAIL`, which exists. Desk build. Expected: `ExitCode` 0. Then, from the exe directory:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "two Runtimes in one process share one scheduler" --rng-seed 1
```

Expected when the accessor is absent: assertion failure, and the message contains `fix #11 is not in this tree`. That is not a compile error. Expected when the accessor exists: PASS. Do not launch Servitor. This case does not change host-visible behaviour until it is unhidden.

- [ ] **Step 3: Confirm the default suite does not run it**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: exit 0. The hidden case is not a failure of that script. If you removed `[.]` because the accessor exists, this script runs the case and it must PASS.

- [ ] **Step 4: Final golden gate**

Delete both `imgui.ini` files. The script path is root-qualified even after a `Set-Location` into the exe directory:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1
```

Expected: 14 lanes, every `diffCount` 0. No `--bless`.

Write one paragraph in the commit message: either "arc not complete: fix #11's process JobSystem is absent" or "arc complete: two Runtimes share ProcessContext's scheduler".

- [ ] **Step 5: Commit**

```
git add Tests/ArcaneTests/src/ProcessSchedulerCompleteTest.cpp
git commit -m "test: record whether two Runtimes share one process scheduler"
```

with the trailer.

---

## Self-review

- Spec 3.5 Done: recycle after fences (Task 2), two-context device-less and `[gpu]` (Task 2), four callers (Task 2), add-only `0..n-1` (Task 2), generated files agree (Task 1), idle counter (Task 2), goldens (Tasks 1, 2, 5, 6).
- Spec 3.6 Done: shared account (Task 4), borrow and reservation (Task 4), the 700+512 case (Task 4), drawn-this-frame (Task 4), two-context destroy (Task 4), footprint pins (Task 5), non-aligned fit (Task 5), dedicated `capacity+16` and `capacity+512` (Task 5), a ring miss whose compiled consumer barrier is skipped and which is not sampled (Task 5), `[gpu]` first-use including oversized (Task 5), goldens (Task 5).
- Spec section 4 step 0: Task 6. The arc is complete only when that case passes without `[.]`.
- Measurement is Task 3, before `Admit` exists. Disk sizes are not resident bytes.
- Revision 4's shared helper is `RingLayout::Fits`, called by `Allocate`.
- No epoch bump. No re-bless.

## Review fixes

Codex review `plan-review-c-gpu-resources.md` (NEEDS REVISION). Each finding is fixed in this plan. None rejected.

1. **A shared slot index leaves the other context's descriptor set unwritten (Critical).** `CreateBindless` (`MeshNode.cpp:288`) builds a table per node, and `AddMaterial` (`MeshNode.cpp:645-650`) updates only that node's `m_bindlessSet`. The device owns the descriptor-array allocation, and writing a slot updates every context's bindless set on that device. The two-context test checks both sets contain the descriptor at that index. Recycling still waits on `AllRetired` across every sampling context.
2. **Classification runs before the ring resets (Critical).** `BeginFrame` is after `BuildFrame` and `Compile` on both `RenderFrame` (1962, 1968, 1981) and `RenderFrameOffscreen` (2111, 2117, 2125). The comment at `NriGraphContext.cpp:1304-1308` forbids setup-time ring allocation. Setup only declares the upload node, the `CopyDst` writes, and the consumer reads. The exec function, after `BeginFrame` on both paths, classifies and allocates. An oversized dedicated payload still does not call `Allocate` and does not bump the overflow count.
3. **Residency identity is Guid only (Critical).** Texture entries and eviction results carry `ColorSpace` (`NriTextureCache.hpp:117` and the key at 323–331). One Guid in both Srgb and Display is two entries. Evicting one space leaves the other. Mesh entries do not use the space. The locked 700 MiB case keeps its distinct guids.
4. **Graph reads were pointed at record calls (Important).** `MeshNode.cpp:1143-1145` and `PickOutlineNodes.cpp:611-612` stay the `CmdSet*` record calls. `builder.Read(..., RgUsage::VertexBuffer)` and `builder.Read(..., RgUsage::IndexBuffer)` go in the setup lambdas: `AddMeshNode` (`MeshNode.cpp:1241`) and `AddPickNodes`' `"pick"` node (`PickOutlineNodes.cpp:1289`), before `Compile()`.
5. **Generated constexpr is unmarked (Important).** Both generators emit `ARC_CONSTANT` on the immediately previous non-empty line and include `Constant.hpp`. HLSL has no marker; the guard scans C++. `ConstantGuardTest` runs before Task 1 commits. No allow-list entry.
6. **Generator is only in the CI scripts, and CI writes before it compares (Important).** The generator runs from `scripts/generate.bat` (before premake at line 56), a new `scripts/generate.sh` that `build.sh` calls, both `compile-shaders.bat` and `compile-shaders.sh` (the shell twin does not execute a powershell line in the bat), and `build.ps1` / `build.sh`. CI uses `-Check` / `--check`, which regenerates into a temp directory and exits non-zero on a diff. It does not write the committed files first.
7. **Bindless tests were comments (Important).** The four cases are executable `CHECK`s that call `DeviceResourceLifetime` and a per-key `Release`. The red is a compile error after the desk build. Today's whole-table `Release` (`BindlessTable.hpp:87-93`) is not executed by that run.
8. **Ring cursor left at an either/or (Important).** `Init` sets the cursor to 0, so `Fits(512, 512)` on a `4 MiB + 511` ring is true until a prefix `Allocate((4ull << 20) - 16, 1)` succeeds. Then `Fits` is false and does not bump the count, and `Allocate(512, 512)` fails and bumps it by exactly 1. No test setter for `m_cursor`.
9. **Red steps ran the old exe (Important).** Tasks 1, 2, 4, and 5 fail the desk build: `kBindlessCapacity`, `DeviceResourceLifetime`, `Admit`, and `FootprintRgba8` / `Fits` are undeclared. Task 3's red is the same kind of compile error (`RenderTextureSettings` undeclared), not "cvars are not registered." Task 6's hidden case compiles, so its red is the assertion `fix #11 is not in this tree` after a successful build. Script paths after `Set-Location` are root-qualified, including `golden-gate.ps1`.

Cross-cutting: the desk build uses `-PassThru`, `PriorityClass = BelowNormal`, and `ExitCode`. Servitor runs after Tasks 1–5 (own `--headless` runtime, `launch` takes no `--client`, exit codes recorded, quit what you launched). Task 1 compares `runtime-scene`. Task 3 explains both new cvars and treats a missing name as a failure. Task 4 requires the host to boot. Task 6 has no Servitor step: the hidden scheduler case does not change a running host.

### Rev 2

Codex re-review `plan-rereview-c-gpu-resources.md` (NEEDS REVISION). Both new findings are fixed. None rejected. All nine previous findings stay as the re-review marked them, including classification after `BeginFrame` (finding 2) and the full refill of 256 valid slots (finding 7).

1. **Ring miss cannot suppress compiled consumers (new, critical).** The decision stays in the upload exec, after `BeginFrame`, on both the window and offscreen paths. On a miss the exec calls `SuppressConsumers`. Later `EmitBarriers` calls (`RenderGraphExec.cpp:1362` and `:1424`) drop that resource's barriers, do not pass them to `CmdBarrier`, and do not update pool carry. The upload entry barrier to `CopyDst` has already been emitted and stays. The consumer exec still runs and does not sample the not-ready entry. A NONE-device frame with a compiled consumer read pins `DebugSkippedBarrierCount() >= 1` and a sample count of 0; the empty-ring frame pins the opposite. `NriUploadRing::Init` is not used on NONE (`NriUploadRing.hpp:25-30`). The ring-hit path still does not suppress, so the same-frame sample stays.
2. **Full-table `Add` never reaches recycling (new).** After the 256-slot refill, `Add` returns `kInvalidSlot` (`BindlessTable.cpp:60-69`). The test checks that refusal, then `Release`s one refilled slot. That slot has no `NoteSample`, so it returns to the free list. A different slot is sampled, released, and left in flight. The next `Add` equals the freed slot, is not `kInvalidSlot`, and is not the in-flight slot.

### Rev 3

Codex re-review `plan-rereview2-c-gpu-resources.md` (NEEDS REVISION). The remaining finding is fixed. None rejected. Full-table recycling stays addressed, as that re-review marked it.

1. **Ring-miss test was a comment (Important).** The case `a ring miss in a frame with a consumer emits no consumer barrier and does not sample` is executable Catch2. Before `Execute` it `REQUIRE`s the mesh node's compiled consumer barrier (`CopyDst` to `ShaderRead`, `preBarriers` size 1, `resourceIndex` 0). The miss frame `CHECK`s `DebugSkippedBarrierCount() == 1`, mesh exec count 1, and sample count 0. The next frame resets the standalone `RingLayout`, does not suppress, `Allocate`s 4096, and `CHECK`s sample count 1, `DebugSkippedBarrierCount() == 0`, and `OverflowCount() == 0`. Tree APIs: `AddNode`, `Compile`, `Execute`, `CreateTexture`, `Write`, `Read`, `RgUsage::CopyDst`, `RgUsage::ShaderRead`, `RgBarrier`, `RingLayout::Init` / `Reset` / `Allocate` / `OverflowCount` (`NriUploadRing.hpp:76-104`), `NriDevice::CreateNoneForTests`, `RgExecuteDesc`, `RenderErrorCount`. Task 5 defines the three the case also calls: `RingLayout::Fits`, `RenderGraphNodeContext::SuppressConsumers(RgTexture)`, `RenderGraph::DebugSkippedBarrierCount`. `CreateTexture` is the realized resource because `ImportTexture(nullptr)` fails `Execute` (`RenderGraphExec.cpp:1257-1284`). The red is a desk-build compile error, `ExitCode` non-zero: `Fits` is absent at `NriUploadRing.hpp:97`, `SuppressConsumers` is absent at `RenderGraph.hpp:225-251`, and `DebugSkippedBarrierCount` is absent beside `DebugSubmitCount` at `RenderGraph.hpp:769`. `logs\build-tests.log` names `SuppressConsumers` and `DebugSkippedBarrierCount`. Do not run `ArcaneTests.exe`.
