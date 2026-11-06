# F5 Plan 1 — Structure Implementation Plan (rev 5.3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the frame a declarative clear, a post-tonemap overlay pass, and one extract stage that publishes an immutable `RenderFramePacket`, while world sprites and world meshes stay pixel-identical.

**Architecture:** Plan 1 does not merge the two world nodes. `batch2d` stays the first world node and clears the canvas with its colour load-op; `mesh` stays next and loads colour while its depth load-op clears to the one depth-convention value. Empty G-buffer, lighting, and TAA nodes are declared and record nothing. After tonemap, one overlay pass samples scene depth and draws the 2D grid, the folded 3D grid, physics debug, the camera rect, and the gizmo. Extract, on the game thread, is the only render-side registry reader; it fills a packet ring of `FramesInFlight()` slots and returns before any GPU work. Sprite submission uses the host's already-registered Batcher2D; extract copies that batcher's drained spans and does not construct one. The GPU never reads packet memory. After Task 9 the overlay pass reads `Packet()->overlay` when a packet is bound, and after Task 8 the outline reads `Packet()->selectedIds` the same way. `FrameDesc::overlay` and `FrameDesc::grid` remain the path only for a frame that did not call `BindPacket`.

**Tech Stack:** C++23, premake5, MSBuild (VS 2026), Catch2 v3, NRI (D3D12 + Vulkan), HLSL via DXC, Astra ECS, PowerShell.

**Spec:** `docs/superpowers/specs/2026-10-09-f5-unified-world-pipeline-design.md`. This plan implements section 8.1 and every ruling that lands in plan 1 (sections 3.1–3.4, 3.7, 4.1–4.4, 7.1). Where this plan and the spec disagree, the spec wins; stop and report.

**Revision:** rev 5.3, 2026-10-15: re-review fixes (`.superpowers/sdd/f5-refresh/final-plan-rereview.md`). Design unchanged. N1 (Important): Task 6 makes the scene-save cover call `GpuSceneInvalidate` after `RenderFrameOffscreen` on every outcome, corrects the "the sync runs once" text, and pins the order with a source-order case. P1 (Important): Task 10 moves the editor's game-HUD production into `RenderSceneToViewport` before `ExtractFrame`, sets `req.gameUi`, and adds a source-order pin plus a GPU witness that a bound packet's `gameUi` reaches the `gameui` node (no plugin in the tree draws a HUD, so an editor Play witness would be empty on today's tree too). N2: Tasks 3 and 5 record the cover's interim overlay gap and keep the cover gizmo-free (`req.gizmoPrims` cleared). N3: the camera rect gets its own collector. N4: `OverlayLine line{};` / `OverlayFill fill{};`. P2: `ArmGraphViewportFrame` copies the packet's light onto `m_meshScene`. L1: each lavapipe run uses a fresh copy of the staged editor dir and `editor-ui` drops the layout seed, as the witnesses do; the volume is read-only and results leave through `/out`. L2: `$image` is the image reference string. L3: one re-run of a failed test seed is allowed; a build failure or a second failure stops.

Rev 5.2, 2026-10-15: Grok and Claude review fixes (`.superpowers/sdd/f5-refresh/grok-plan-review.md`, `claude-plan-review.md`; controller rulings in `rulings.md`, "Review fix-pass rulings"). Design unchanged. Grok: G-C1 Task 9 physics overlay draws in Edit and Play through `packet.spriteView` with `opts.alpha = RenderContext2D::alpha`, plus Play rows in the selected-body test (same fix as C-I5); G-I1 Task 2 census table rewritten for every index-bearing `DeclareGraphFrame` assertion (`:5858-5976`, `:6590-6619`, `:7594-7604` added; the two `AddMeshNode` cases unchanged); G-I2 letterbox copies `AddBatch2DNode`'s setup incl. `SetColorAttachments`; G-I3 delete `Batch2DNode.cpp:1395-1406` only; G-I4 Task 3 spells all three grid frame-shape sections; G-I5 Task 3 producers emit pixel-space (`screenSpace = true`) primitives through `OverlayBatchCollector` (same fix as C-I3); G-I6 `DepthConvention.hpp` included at file scope; G-I7 `EditorApp.hpp` in Task 3's Files and `git add`; G-I8 Task 13 Step 3b is a concrete CI-image recipe with an explicit copy-out (same fix as C-I7); G-I9 Task 13 lane extras quoted from `$combos` and every blessed PNG copied back; G-M1 the formula case is labelled as a numeric pin. Claude: C-C1 extract keeps today's two-view cull (`meshVisible`, `views[1]`, no-mesh-view arm) with a pinning test (Tasks 4, 6); C-C2 `CollectPickables` stays exported in `namespace Arcane`; C-C3 perspective witness uses the XY plane; C-I1 no-camera fallback is `RenderContext2D::view`, `spriteViewFromScene` gates `SetView` (Tasks 4, 5, 6, 7, 10) with a test; C-I2 ImGui test windows set position and size so they render on their first frame (Tasks 4, 10); C-I3 see G-I5; C-I4 `BuildGizmoOverlayPrims` runs before `RenderSceneToViewport` from Task 3; C-I5 see G-C1; C-I6 `BuildExtractRequest` and the scene-save cover extract and bind a packet, and both join the host gate (Tasks 5, 12); C-I7 see G-I8; C-I8 helper paste range `:880-927`; C-I9 the host gate strips comments over the whole file before brace-matching and skips literals, with a self-test. Minors fixed: C-M1 (test includes), C-M2 (`PresentChromeFrame` body end), C-M3 (regression-pin wording), C-M4 (`tonemap.hlsl:37-50`), C-M5 (`m_overlay` ownership and `Release`), C-M6 (`Arcane/Math/Aabb.hpp`), C-M7 (post-extract order), C-M8 (physics body moved off the grid line), C-M9 (third "20" in the spelling guard), C-M10 (`%(` include items skipped), C-M12 (`ARC_CONSTANT` notes). No change needed: C-M11, C-M13 (the findings record correct text). Deferred: C-M14 (split Tasks 3 and 9; task numbering kept). Rejected: none.

Rev 5.1, 2026-10-15: the controller rulings in `.superpowers/sdd/f5-refresh/rulings.md` are applied (PR-1 A, PR-2 A, PR-3 A through the CI image only; `ExtractRequest::physicsSelectedBody` drives `opts.onlyBody`; extract carries both `PushSceneCamera` warnings; `SceneCacheRecords.hpp` joins the spelling guard).

Rev 5, 2026-10-15. Re-anchored to the modular tree after `origin/main` (engine modularization PR #10 and everything since) was merged into this branch at `b3d29a93`; the design is unchanged. Every cited path moved to `Source/<Type>/<Module>/src/...` or `Tests/ArcaneTests/src/...`, every `file:line` was re-derived against the merged tree (lines that still say what rev 4.1 claimed were re-numbered; the ones whose code changed are rewritten and listed in the refresh report), outputs are under `Binaries\`, and the build, suite, and golden commands are `scripts\ci\build.ps1`, `scripts\run-tests.ps1`, and `scripts\golden-gate.ps1`. New files are placed in the lowest module that owns them, carry that module's export macro, and need no premake or `.arcmodule` edit (module sources are globbed). Symbols renamed on `main` are renamed here: `PhysicsResource` is `PhysicsWorld2D` (solver behind `Detail::Physics2D::Access`), `PhysicsInterpBuffer`/`InterpPose`/`InterpSlot` are `PhysicsInterpBuffer2D`/`PhysicsInterpPose2D`/`PhysicsInterpSlot2D`, `PhysicsDebugDrawOptions` is `PhysicsDebugDrawOptions2D`, `ImGuiHud()` returns a `RenderOverlayNode*`. Three placement/gate questions were ruled (rev 5.1): the ImGui snapshot's code leaves `RenderFramePacket.hpp` for ArcaneImGui (PR-1), the include gate's TU set after the Render/ImGui/Physics2D split (PR-2), and the lavapipe golden slot in Task 13 (PR-3). Report: `.superpowers/sdd/f5-refresh/refresh-report.md`. Rev 4.1 (Task 9 DrawGrid2D call fixed after the Codex verify) and the Grok GX1–GX5 / earlier-round content are otherwise unchanged.

## Global Constraints

- **Worktree:** `D:\dev\starworks\Arcane-f5`, branch `feat/f5-unified-world`. Never push, never force-push, never merge to `main`.
- **Scope:** plan 1 only. Plan 2 (Surface materials, preview-as-sprite-row, greyed pins) and plan 3 (one world pass, shared sprite list, primary camera, `runtime-scene` two-mesh re-bless, 50k budget, view-space slab witness) are out of this document. See "Out of scope" below.
- **Module placement (rev 5).** Modules follow `docs/superpowers/specs/2026-10-11-engine-modularization-design.md` (s1, s2.3): a module may depend only on what its `.arcmodule` declares; a `Runtime` module depends only on `Runtime` modules, a `Client` module on `Runtime` and `Client` modules; no cycles; ImGui stays optional per host behind ArcaneRender's `RenderOverlayHooks` seam (`Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderOverlayNode.hpp`), so ArcaneRender never includes `<imgui.h>`. Include paths did not change for the files this plan touches, except that the ImGui node is now `<Arcane/ImGui/ImGuiNriNode.hpp>` in ArcaneImGui. Each engine module's project globs `src/**.hpp` and `src/**.cpp` (`build/modules.lua:397`), so a new file needs no `premake5.lua`, `build/modules.lua`, or `.arcmodule` edit; none of the placements below adds a module dependency. Tests stay in the one `Tests/ArcaneTests` exe (its `files` glob is `premake5.lua:1239-1241`; it links every engine module, `:1229`). New files:

| New file | Module (type) | Export macro |
|---|---|---|
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/DepthConvention.hpp` | ArcaneScene (Runtime), beside `SceneCamera.hpp` | none (header-only `constexpr`) |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCameraView.hpp` | ArcaneScene | none (header-only struct) |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp` | ArcaneScene, beside `SceneResources.hpp` | none (header-only) |
| `Source/Runtime/ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp`, `.cpp` | ArcaneCore (Runtime) -- the lowest module both ArcaneRender and ArcaneServer reach | `ARC_CORE_API` (`<Arcane/Core/Api.hpp>`) |
| `Source/Client/ArcaneRender/src/Arcane/Render/DepthConventionMap.hpp` | ArcaneRender (Client) -- needs NRI | none (`inline`) |
| `Source/Client/ArcaneRender/src/Arcane/Render/GridSceneDesc.hpp`, `OverlayPrimitive.hpp`, `VisibleSet.hpp`, `PostChainDesc.hpp` | ArcaneRender | none (header-only) |
| `Source/Client/ArcaneRender/src/Arcane/Render/OverlayBatchCollector.hpp` (rev 5.2) | ArcaneRender | none (header-only `Batcher2D` subclass; its base is `class ARC_RENDER_API Batcher2D`) |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.hpp`, `.cpp` | ArcaneRender | `ARC_RENDER_API` (`<Arcane/Modules/ArcaneRenderApi.hpp>`) on `class OverlayNode` and `AddOverlayNode`, as `GridNode.hpp:158`/`:283` |
| `Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.hpp`, `.cpp` | ArcaneRender | `ARC_RENDER_API` on `RenderFramePacket::Clear` and `class PacketRing` |
| `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImDrawSnapshot.hpp`, `.cpp` (ruling PR-1) | ArcaneImGui (Client) | `ARC_IMGUI_API` (`<Arcane/Modules/ArcaneImGuiApi.hpp>`) on `CopyImDrawSnapshot` and `class ImDrawSnapshotRebuild` |
| `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.hpp`, `.cpp` | ArcaneRender -- the spec's extract TU stays in the render tree | `ARC_RENDER_API` on `ExtractFrame` |
| `Source/Client/ArcaneRender/src/Arcane/Render/Grid2D.hpp`, `.cpp` | ArcaneRender (moves down from the ArcaneEditor program) | `ARC_RENDER_API` on `PixelsPerMetre`, `PlanGrid2D`, `EmitGrid2D` |
| `data/shaders/tonemap_operator.hlsl`, `data/shaders/overlay.hlsl` | shaders (compiled by ArcaneRender's prebuild) | -- |
| `Tests/ArcaneTests/src/*Test.cpp` (six new files) | ArcaneTests | -- |

  A new member function of an already-exported class (`class ARC_RENDER_API RenderGraph`, `RenderGraph.hpp:496`; `class ARC_RENDER_API NriGraphContext`, `NriGraphContext.hpp:319`) needs no macro. `inline` functions in headers (`BuildGpuSceneFrame`, `MeshSectionRanges`) need none. Astra calls made inside ArcaneRender (ExtractFrame's `GetResource` / `CreateView`, the `GpuSceneSync`, `BuildVisibleSet`, and `RenderSubmissionSystem` bodies it instantiates) run on the process type context because every module image attaches through `AttachAllModules` (`Source/Runtime/ArcaneCore/src/Arcane/Core/ModuleAttach.hpp`, ruling R13); `CollectPickables` already reads the registry from ArcaneRender today.
- **ABI:** `kGamePluginABIVersion` is 59 on `main` at this revision (`Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp:10`; `Source/Runtime/ArcaneEngine/src/Arcane/Plugin/PluginABI.hpp` includes that header). This plan edits neither file. The packet is an internal render type; the struct moves (`SceneCameraView`, `VisibleEntry`/`VisibleSet`, the `SceneResources.hpp` records) keep every layout. If an executor is ever forced to bump, the number is **the next ABI after `main` at execution time** -- read `AbiVersion.hpp` on `origin/main` first; the unmerged Servitor branch (PR #13) takes 60–61 -- never a number written in this plan.
- **Every commit message ends with:**

```
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

- **Style:** `/MD` everywhere, no `/fp:fast` (module projects build `floatingpoint "Strict"`, `premake5.lua:318`), MKS units, ASCII comments, UTF-8 without BOM. New tunables are settings. The depth convention is an enum selected in code (`DepthConvention::ForwardZ`); it is not a cvar. `render.clearColor` stays the canvas clear (`CVarColor` default `{0.02f, 0.02f, 0.04f, 1.0f}`, `RenderDeviceSettings.hpp:83`).
- **Never edit vendored copies in place:** `ThirdParty/Astra`, `ThirdParty/Manifold2D`, `ThirdParty/Mosaic`, `ThirdParty/NRI`, `ThirdParty/imgui`.
- **Do not edit** `ReferenceProject/Content/scenes/main.arcscene`, `f3_cull_blend.arcscene`, or any research doc. Camera selection is unchanged: through plan 2 extract still applies today's split (sprites via `ActiveSceneCamera`, meshes via `ActivePerspectiveSceneCamera`).
- **Build with the PowerShell tool**, one entry point for every configuration `<Cfg>` (`Debug`, `Release`, or `Dist`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 <Cfg>
```

`scripts\ci\build.ps1` is THE Windows build sequence (`.github/workflows/ci.yml` and `scripts\ci\local.ps1` run exactly this): generate (`GenerateProjects.bat` -> `scripts\generate.bat` -> premake `vs2026`), `Arcane.slnx`, `arcbuild build --project ReferenceProject --config <Cfg> --sdk <this checkout>`, then `Arcane.slnx` again so every host restages the game module. It stops at the first failing step; logs are `logs\build-{generate,engine,referencegame,tests}.log`. Because it always regenerates, a new `.cpp`/`.hpp` needs no separate generate step. ArcaneRender compiles with `ARC_RENDER_EXPORTS` (`build/modules.lua:382`), `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1` and `VK_USE_PLATFORM_WIN32_KHR` (`premake5.lua:364-369`), and the module defaults `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `/Zc:__cplusplus`, `/bigobj` (`premake5.lua:317-323`). Its prebuild runs `data/shaders/compile-shaders.bat` (`premake5.lua:370`); on Linux/macOS `data/shaders/compile-shaders.sh` reads the `.bat`'s `call :compile` lines (`compile-shaders.sh:7-10`), so a shader entry is added once, to the `.bat`.

- **The single game-module slot.** `ReferenceProject\Binaries\ReferenceGame.dll` holds one configuration. `build.ps1`'s game step is `arcbuild build`, which forces a rebuild when the slot holds the other configuration's DLL, and its last step restages the hosts, so a config flip is one `build.ps1 <Cfg>` call. `--sdk` is always this checkout; do not rely on `ARCANE_SDK`. The last command of Task 13 is `scripts\ci\build.ps1 Debug`, so the slot ends as a Debug DLL.

- **Tests run from the exe directory:**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\Binaries\<Cfg>-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[tag]"
```

Record the `Randomness seeded to:` line. GPU tests are tagged `[gpu]`. Device-less graph tests are `[nri]`. `Arcane::Test::RepoRoot()` is `Tests/ArcaneTests/src/Helpers/ConstantScan.hpp:34`. **Suite gate before every task's commit:** `powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug` -- CI's test entry point: `~[gpu]` from the exe dir, JUnit + JSON under `test-results\`, then `scripts\check-baselines.ps1` against `scripts\automation-baselines.json` (a count that rises is fine; a drop fails). The task's own `[gpu]` filters are what run the GPU cases.

- **Tri-platform CI.** `.github/workflows/ci.yml` builds windows-msvc, linux-gcc-14, linux-clang-19, and macos-appleclang x Debug/Release/Dist, runs `~[gpu]` on every leg, runs `[gpu]` on Mesa lavapipe (linux-gcc-14 Debug) and on MoltenVK (macOS Debug), and boots the monolithic package on Release. Every file this plan adds compiles on all four toolchains: Windows-only test code (`_popen`, `vswhere`, `cl.exe` in Task 12) sits under `#if defined(_WIN32)` (the convention in `AgilitySdkTest.cpp`, `HostConfigTest.cpp`); a spawned host is named through `Arcane::Platform::ExecutableFileName` (`ServerWitnessTest.cpp:5`, `:19`); D3D12 GPU cases skip through `ARC_REQUIRE_BACKEND` (`Helpers/GpuCapability.hpp:50-56`). `scripts\ci\local.ps1` runs windows-msvc Debug and linux-gcc-14 Debug (in the CI image) on this desk; Task 13 runs it.

- **Dist and verify guards.** Code a task moves keeps its guard. `--pick-probe` is `#if !defined(ARC_BUILD_DIST)` (`RuntimeFrame.cpp:554-590`). The `pick@` probe block (`RuntimeFrame.cpp:617-652`), the `--headless` offscreen vehicle (`RuntimeApp.cpp:502-581`), `--compare` setup (`:650-707`), the shutdown pick resolution (`:970-1099`), and the `--report` / `--compare` / `--bless` tail of `ShutdownGraphPath` (`:1186-1564`) are `#if defined(ARC_RUNTIME_HOST_VERIFY)`: ArcaneRuntimeHost is a static library that ArcaneRuntime.exe links with the verification vehicle and a monolithic package links without it (`premake5.lua:436-471`). Build Dist once at the end (Task 13) because CI does.

- **Goldens are Task 13 only.** Tasks 1–12 do not run `scripts\golden-gate.ps1`. After Task 3 the editor grid pixels differ from the blessed PNGs on purpose; `editor-ui` stays red until Task 13's explained re-bless. World lanes (`runtime-scene`, `f3-cull-blend`) must stay `diffCount == 0` with no re-bless. Before any golden run, delete `imgui.ini` in both host exe dirs (`Binaries\<Cfg>-windows-x86_64-md\ArcaneRuntime` and `ArcaneEditor`). Re-bless only the grid/overlay class, and only with the written note in Task 13. A missing 2D grid, 3D grid, physics-debug line, or camera rect is a failure, not a bless. Bless from the host exe directory (the project resolves to the staged copy), then immediately copy the blessed PNGs from the staged `Verify\References` into source `ReferenceProject\Verify\References`. `git status` must show those PNGs. Gate Debug and Release. Do not bless again under Release. Do not bless `inprocess-lit-cube` (it has no bless mode). If mesh thumbnails move, stop: the tonemap include refactor must stay pixel-identical on world content. The reference tree now has an adapter level: `ReferenceProject/Verify/References/vulkan-lavapipe/` holds five editor PNGs that CI's lavapipe leg compares against (`ReferenceAdapterSet`, `Source/Developer/ArcaneVerify/src/Arcane/Host/ReferenceImages.cpp:36`); Task 13 covers them (ruling PR-3).

- **Nothing on the frame path blocks the main thread.** Extract returns before `RenderFrame` / `RenderFrameOffscreen`. The `/showIncludes` compile is a Catch2 test, not a per-frame step. No `DeviceWaitIdle` is added.

- **Packets and the GPU.** `kMaxFramesInFlight` is 3 (`FramePacing.hpp:39`). `FramesInFlight()` is the latched depth (`FramePacing.hpp:43`). `render.framesInFlight` defaults to 2 and its range is 2..3 (`RenderDeviceSettings.hpp:81`, `:110`). The packet ring stores `kMaxFramesInFlight` slots and uses the live `FramesInFlight()` depth. The GPU reads `NriUploadRing` copies (`NriUploadRing.hpp:22`, `:136`, `NriUploadRing.cpp:102`), never packet memory.

## Review Focus

These are the five inputs most likely to bite that a task's other tests would miss. Each one has a pinning test in the task named, written in that task's step style.

1. **The chrome graph declares a world pass.** `PresentChromeFrame` (`EditorAppFrame.cpp:4026-4045`) sets only `frame.imgui` today, and `DeclareGraphFrame` always starts with `batch2d` (`NriGraphContext.cpp:1283`), so an imgui-only shape clears a canvas through an empty batcher and then tonemaps it (`RenderGraphTest.cpp:6177-6220`). Plan 1 adds `RgFrameShape::world` / `FrameDesc::world`, default `true`. Chrome sets `frame.world = false` and gets a letterbox raster (colour load-op CLEAR to `render.clearColor`, no draws), then tonemap, then imgui. Forbidden names on that shape: `batch2d`, `mesh`, `grid`, `overlay`, `gbuffer`, `lighting`, `taa`, `gpuscene-sync`, `mesh-cull`. An empty-batcher *world* frame still declares `batch2d`. Pinned in Task 2 (`"chrome world=false is letterbox, tonemap, imgui"` and `"PresentChromeFrame sets frame.world = false"`).
2. **A frame still in flight holds the previous packet while extract writes the slot `FramesInFlight()` extracts old.** The GPU never reads packet memory. After `FramesInFlight() - 1` further extracts the first packet's bytes are intact; the next extract overwrites that slot. A span taken from the packet lies inside the packet's own vector. Mutating the source component after extract leaves the packet unchanged. Render origin translation is `{0,0,0}`. The packet header includes `SceneCameraView.hpp` and keeps `spriteView` as `std::optional<SceneCameraView>`. The ImGui snapshot is an owned per-list copy inside that same slot; its types are Render-owned plain data and the copy/rebuild code lives in ArcaneImGui (ruling PR-1). A user texture (`TexRef._TexData == nullptr`) stores `ImTextureID` as `ImU64`. A managed command stores the `ImTextureData*`, and the snapshot also carries the `ImDrawData::Textures` pointer list, so declaration-time `NewFrameTexUpdates` can `SetTexID` before record. `ImDrawSnapshotRebuild` assigns `ImDrawData` fields directly and never calls `AddDrawList`. List boundaries are kept. A callback command refuses the whole snapshot. Pinned in Task 4 (`"a packet slot stays intact until FramesInFlight extracts have passed"`, `"an ImDrawSnapshot matches the live draw data field for field and refuses a callback"`, and `"an ImDrawSnapshot rebuild reaches ImGuiNri"`).
3. **An empty post chain is not pixel-identity for a blended overlay, and a post chain does not grade an opaque overlay primitive.** Blending a partial-alpha linear colour into scene colour and then running the tonemap differs from running the tonemap operator on the primitive and blending after, including when the chain is empty (C3). Separately, an opaque overlay primitive (alpha 1) reads the same bytes with a scene-tinting post chain and without one, while a world pixel in that same pair of frames differs. Pinned in Task 3 (`"tonemap-then-blend differs from blend-then-tonemap"` and `"an opaque overlay pixel is unchanged by a post chain"`). Task 13 carries the explained re-bless note; it does not claim the empty-chain grid matches today's golden.
4. **A rendering host runs extract once per frame. A non-rendering host leaves the counter at 0.** `RuntimeApp --headless` builds an offscreen vehicle and draws (`RuntimeApp.cpp:451-461`; the vehicle is built at `:502-581`, inside `#if defined(ARC_RUNTIME_HOST_VERIFY)`, which ArcaneRuntime.exe defines); that process prints `extract-frames: N` after `--frames N`. `Arcane::Runtime` has no client and an empty render scheduler (`ClientRuntimeTest.cpp:26-36`); `Loop().Advance` (`RunLoop.hpp:109`) leaves `ExtractFrameCallCount()` at 0. `ArcaneServer` links ArcaneEngine, ArcaneNet, and ArcaneCrypto, whose closure reaches ArcaneCore and no Client module (`premake5.lua:884-887`), ticks with `Loop().Advance` (`ServerApp.cpp:191`), and writes that same Core counter into `ServerReport::extractFrameCalls`, which stays 0. Pinned in Task 11 by running those three hosts. Source text is not the proof.
5. **Overlay class, colour space, a missing depth target, and perspective grid depth.** Each primitive is depth-tested or always-on-top (spec section 3.1). A depth-tested line behind an opaque mesh is hidden; the same line tagged always-on-top is visible. The 2D grid is depth-tested and, because sprites write no depth in this plan, draws over sprites (that crossing is part of the Task 13 note). The 3D grid uses nearer-or-equal and does not write depth. Physics debug does not call `SetLayer` today (`EditorAppFrame.cpp:1902-1991`) and must be tagged always-on-top explicitly. Gizmo colours are already display-referred (`GizmoOverlay.hpp:26-45`, `ColorConvertFloat4ToU32`); a display-referred red `(1,0,0)` reads back unscaled and is not `ACES(1,0,0)`. Linear primitives go through ACES then true sRGB with no exposure term (`tonemap.hlsl:36-56`). With no mesh pass, today's grid comment (`NriGraphContext.cpp:1380-1384`) draws unoccluded and does not attach the placeholder depth; the overlay does the same and does not sample a missing texture. The screen-space witness writes `clip.w = 1`, so a second divide of `SV_Position.z` by `SV_Position.w` still compares the right value on those lines. A perspective grid fragment does not: in the pixel shader `SV_Position.z` is already post-divide and `SV_Position.w` is `1/clip.w`. Both overlay pixel shaders use `input.clip.z`. Pinned in Task 3 (`"depth-tested hides, always-on-top shows, display-referred red is not ACES, missing depth draws unoccluded"` and `"a perspective grid axis is hidden on the cube and visible beside it"`). Task 9 pins the host path those cases do not: extract, `BindPacket`, present, and a 2D-grid pixel, a gizmo pixel, and a physics-debug pixel that are absent when the pass still reads only `FrameDesc`. A missing class is not a Task 13 bless.

## Out of scope

Do not implement any of these in this plan. A task that starts one of them is wrong; stop.

- Plan 2: Surface domain, `.arcmat` migration, vertex factories, the six mesh shaders becoming one material, preview rows, checkerboard moving off the world quad. The checkerboard stays a world quad (spec section 5.7). `Impl::Harvest` and `ShaderEditorDocument::RenderGraphPreview` contain no forbidden registry calls today; they must not grow any, and they do not have to call `ExtractFrame` when they author a local quad.
- Plan 3: one world raster pass, the shared back-to-front sprite list, sprite depth writes, quantized sort keys, sprite texture/cutoff, the 50k budget, sprite-edge parity, `Camera::primary`, editing `main.arcscene` or `f3_cull_blend.arcscene`, the perspective world-mesh occlusion witness, the view-space slab witness, the `runtime-scene` two-mesh re-bless. Task 3's perspective witness is the overlay grid and a world-space overlay line. It does not re-bless `runtime-scene`.
- Selecting reverse-Z. The enum value exists so a later change is one assignment. This plan selects `ForwardZ`.
- An exposure multiply. Overlay conversion is the tonemap operator alone. When D1 adds exposure it scales scene colour and does not scale overlay primitives. `overlay.hlsl` has no `exposure` identifier.
- A velocity target, TAA jitter, or filling the G-buffer, lighting, or TAA slots. Those three nodes are empty.
- Rebasing positions to a non-zero render origin, or rewriting rows into camera-relative positions. Origin translation is `{0,0,0}` until D17.
- Retargeting the pick pass onto a shared list. Pick compare (`PickOutlineNodes.cpp:582`) reads the nearer-wins op. Pick's own `CmdClearAttachments` (`PickOutlineNodes.cpp:400`) stays.
- Deleting `AddGridNode`. The frame stops calling it. The direct declaration test at `RenderGraphTest.cpp:7210` still calls it and must keep compiling.
- An ABI bump, a `PluginABI.hpp` edit, or a change to which camera draws.

## File map

| File | Task | Responsibility |
|---|---|---|
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/DepthConvention.hpp` (new) | 1 | `DepthConvention`, `kSelectedDepthConvention`. No Astra, no NRI. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCamera.hpp` | 1, 4 | Task 1 includes `DepthConvention.hpp` above the DEPTH CONVENTION comment (`:120`). Task 4 deletes `struct SceneCameraView` (`:31-42`) and includes `SceneCameraView.hpp`. `ActiveSceneCamera` (`:57`) still returns `std::optional<SceneCameraView>`. `ActivePerspectiveSceneCamera` (`:206`) still returns `std::optional<ViewTransform>`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/DepthConventionMap.hpp` (new) | 1 | `DepthClearValue`, `NearerWinsOp`, `NearerOrEqualOp`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp` | 1, 2 | Compare at `:832` reads `NearerWinsOp`. Depth clear sites `:83`, `:955`, `:1309` read `DepthClearValue` in Task 2. `CmdClearAttachments` at `:942-957` deleted in Task 2. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp` | 1, 8 | Task 1: compare at `:582` reads `NearerWinsOp`. `:400` stays. Task 8: `AddOutlineNodes` (`:1371-1372`) calls `PrepareSelection` with `Packet()->selectedIds` when a packet is bound, and `CurrentSelectedIds()` (`NriGraphContext.hpp:1070`) when it is not. |
| `Tests/ArcaneTests/src/DepthConventionTest.cpp` (new) | 1 | Mapping checks and the source scan of the two compare sites. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraph.hpp` | 2 | `RgLoadOp`, `RgAttachmentOp`, setters, queries. `NodeDesc` (`:853-867`). |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraph.cpp` | 2 | Store the ops beside `SetColorAttachments` (`:211` region). |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraphExec.cpp` | 2 | Fill `nri::AttachmentDesc` (`:1368-1382`) from the ops. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp` | 2, 3 | `FrameDesc::world`, `RgFrameShape::world` (default true). Task 3 adds the overlay list pointer. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp` | 2, 3, 9 | `DeclareGraphFrame` (`:1237`) and `BuildFrame` (`:1640-1672`). Task 9's `wantOverlay` and the pointers passed to `AddOverlayNode` come from `Packet()->overlay` when a packet is bound. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp` | 2, 5 | Delete the colour `CmdClearAttachments` (`:1395-1408`). Task 5 feeds the packet's owned spans and still `Prepare`s them against `CurrentBatch()` (the host batcher). |
| `Tests/ArcaneTests/src/RenderGraphTest.cpp` | 2, 3 | Every `DeclareGraphFrame` node census. Task 3 moves the grid census to `overlay`. |
| `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` | 2, 3, 5, 7, 8, 9, 10 | `PresentChromeFrame` (`:4026`) sets `world = false` in Task 2. Task 3 retargets grid/physics/camera/gizmo submissions. Task 5 calls `ExtractFrame` after `SubmitSceneToBatcher` returns. Tasks 7–10 move the named builder reads into extract. |
| `Source/Programs/ArcaneEditor/src/App/EditorApp.hpp`, `EditorApp.cpp` | 3, 5, 9 | Task 3: `m_overlayPrims`, `m_gizmoPrims`, `BuildGizmoOverlayPrims`. Task 5: `m_packetRing`, `BuildExtractRequest`, and the scene-save cover (`CaptureGraphViewportPng`, `EditorApp.cpp:2056-2090`) extracts and binds a packet. |
| `Source/Client/ArcaneRender/src/Arcane/Render/GridSceneDesc.hpp` (new) | 3 | `GridSceneDesc` moved out of `GridNode.hpp:96-156`. No NRI, no Astra. |
| `Source/Client/ArcaneRender/src/Arcane/Render/OverlayPrimitive.hpp` (new) | 3 | Overlay enums and primitive records. |
| `Source/Client/ArcaneRender/src/Arcane/Render/OverlayBatchCollector.hpp` (new) | 3, 9 | `Batcher2D` that records `Line`/`Circle`/`Rect` as overlay primitives: the editor's Task 3 producers, then extract's physics debug. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.hpp`, `.cpp` (new) | 3, 9 | Node `"overlay"`. Task 3 builds the `b0`/`t0`/`s0` layout and the depth SRV. Task 9's `Record` reads `Packet()->overlay` when a packet is bound. |
| `data/shaders/tonemap_operator.hlsl` (new), `data/shaders/tonemap.hlsl`, `data/shaders/overlay.hlsl` (new), `data/shaders/compile-shaders.bat` | 3 | Shared ACES + sRGB. Overlay entry points. |
| `Tests/ArcaneTests/src/OverlayWitnessTest.cpp` (new) | 3 | Review Focus 3 and 5. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCameraView.hpp` (new) | 4 | `SceneCameraView`. Includes `ViewTransform.hpp`. No registry. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp` (new) | 12 | `Detail::Physics2D::Lerp`, `Detail::Physics2D::AngleLerp`, `PhysicsInterpPose2D`, `PhysicsInterpSlot2D`, `PhysicsInterpBuffer2D`, `SpriteEntry`, `MeshEntry`, `ResolvedMeshMaterial`. No `Ecs.hpp`, no `Registry.hpp`. `SceneResources.hpp` includes it. |
| `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneResources.hpp` | 12 | Keeps `SceneRoot`, `RenderContext2D`, `SpriteTable`, `MeshTable`, `MeshMaterialTable`. Includes `SceneCacheRecords.hpp` and still includes `Ecs.hpp`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/VisibleSet.hpp` (new) | 4 | `VisibleEntry`, `VisibleSet`. No `Registry.hpp`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/VisibilitySystem.hpp` | 4 | Includes `VisibleSet.hpp`. `BuildVisibleSet` (`:119`) stays here. The header's own `#include <Astra/Registry/Registry.hpp>` (`:30`) stays; only `ExtractFrame.cpp` may include this header from a Render `.cpp`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.hpp`, `.cpp` (new) | 4 | Packet, `PacketRing`, and the Render-owned plain-data `ImDrawSnapshot` / `ImDrawListSnapshot` types. No `<imgui.h>` (ruling PR-1). |
| `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImDrawSnapshot.hpp`, `.cpp` (new) | 4, 10 | `CopyImDrawSnapshot`, `ImDrawSnapshotRebuild`, and the `static_assert`s that pin the Render-owned types to imgui's layouts (ruling PR-1). |
| `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderOverlayNode.hpp`, `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiHostHooks.cpp` | 4 | `RenderOverlayHooks` gains `snapshot`; `InstallImGuiHostHooks` sets it to `CopyImDrawSnapshot` (ruling PR-1). |
| `Tests/ArcaneTests/src/RenderFramePacketTest.cpp` (new) | 4 | Review Focus 2, camera field type, field-for-field ImGui snapshot. |
| `Tests/ArcaneTests/src/NriGraphPixelTest.cpp` | 4, 5, 6, 8, 9 | Task 4 appends the ImGui rebuild reach case. Task 5 appends the registered-material pixel witness. Task 6's existing `BuildGpuSceneFrame` call updates stay. Task 8 appends the packet-selection polarity case. Task 9 appends the extract-to-present overlay case. |
| `Source/Runtime/ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp`, `.cpp` (new) | 5 | `NoteExtractFrameCall`, `ExtractFrameCallCount`, `ResetExtractFrameCallCount`. Core, so ArcaneServer links it. |
| `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.hpp`, `.cpp` (new) | 5–10 | The one Render `.cpp` allowed to include `Registry.hpp`. Each task adds one producer family. |
| `Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.cpp` | 5 | `InstallRenderSystems` (`:47-51`) stops adding `RenderSubmissionSystem`. |
| `Tests/ArcaneTests/src/ClientRuntimeTest.cpp` | 5 | The two `HasSystem<RenderSubmissionSystem>` checks on `ClientRuntime` become `CHECK_FALSE`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/GpuSceneSync.hpp` | 6 | `BuildGpuSceneFrame` (`:285`) takes `std::span<const MeshSectionRange>`. Header include of `Registry.hpp` (`:27`) stays; only `ExtractFrame.cpp` includes this header from a Render `.cpp`. |
| `Source/Client/ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp` | 6 | `PrepareSceneForRender` (`:87-111`): only its `BuildGpuSceneFrame` call (`:110`) changes, to the `MeshSectionRanges` wrapper. The hosts stop calling it and the function stays for the tests that call it. `ExtractFrame.cpp` does not call it (rev 5): this header is ArcaneClient's, and ArcaneRender may not include ArcaneClient (ArcaneClient depends on ArcaneRender); Task 6 already does its steps inline in extract. |
| `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp` | 5, 6, 7, 8 | One `ExtractFrame` above `:753` from Task 5. `SetRenderContext` stays in front of that call. Later tasks delete the producer calls this function used to make. `GpuSceneInvalidate` at `:761` stays. |
| `Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp` | 7, 11 | Task 7 moves the no-camera `CreateView` (`:886-890`) into extract. Task 11 prints `extract-frames:` once at shutdown. |
| `Source/Client/ArcaneRender/src/Arcane/Render/PickEmit.cpp` | 8 | Move `RotateVec` (`:24-29`) and `CollectPickables` (`:32` through the line before `PickBoundHalfExtents` at `:227`). Drop `SpriteGeometry.hpp` (`:4`), `VisibilitySystem.hpp` (`:5`), `Components.hpp` (`:6`), `PhysicsComponents.hpp` (`:7`), `PhysicsSystem.hpp` (`:8`), `SceneResources.hpp` (`:9`), `Registry.hpp` (`:11`), and `<utility>` (`:17`). `PickPassId` (`:189`), `PickSampleTexel` (`:199`), `PickKindCode` (`:214`), `PickBoundHalfExtents` (`:227`), and `BuildPickIdGeometry` (`:244`) stay. |
| `Source/Client/ArcaneRender/src/Arcane/Render/Grid2D.hpp`, `.cpp` (new) | 9 | `PlanGrid2D` / `EmitGrid2D`, moved from the editor. |
| `Source/Programs/ArcaneEditor/src/Viewport/ViewportGrid.hpp`, `.cpp` | 9 | `DrawGrid2D` forwards to `EmitGrid2D`. |
| `Source/Client/ArcaneRender/src/Arcane/Render/RenderSystems.hpp` | 5 | `operator()` from `:50` through its closing brace is what extract runs, on the host `RenderContext2D::batcher`. Extract does not replace that pointer. The header's `Registry.hpp` include (`:37`) stays. Only `ExtractFrame.cpp` includes this header from a Render `.cpp`. |
| `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp` | 10 | `Record` (`:58`) rebuilds `ImDrawData` from the packet snapshot. |
| `Source/Programs/ArcaneServer/src/ServerReport.hpp`, `.cpp`, `Source/Programs/ArcaneServer/src/ServerApp.cpp` | 11 | `extractFrameCalls` written from `ExtractFrameCallCount()` after the tick loop (`ServerApp.cpp:225-230`). |
| `Tests/ArcaneTests/src/ExtractFrameTest.cpp` (new) | 5–10 | One Catch2 case per producer family. |
| `Tests/ArcaneTests/src/ExtractHostTest.cpp` (new) | 11 | Review Focus 4. Runs the three hosts. |
| `Source/Client/ArcaneRender/src/Arcane/Render/MeshCache.hpp`, `MeshMaterialCache.hpp`, `SpriteCache.hpp`, `SpriteCache.cpp`, `PhysicsDebugDraw.hpp` | 12 | The three cache headers include `SceneCacheRecords.hpp` instead of `SceneResources.hpp`. Render's `PhysicsDebugDraw.cpp` already preprocesses neither (the physics-debug GATHER half, with the `SceneResources.hpp` include, moved to `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.cpp:20`); only the comment at `PhysicsDebugDraw.hpp:50` names the new header. |
| `Tests/ArcaneTests/src/RenderRegistryIncludeTest.cpp` (new) | 12 | `/showIncludes` gate, including a per-TU check of the four cache/debug-draw TUs, and the comment-stripping host frame-builder gate. |
| Golden PNGs under `ReferenceProject/Verify/References` | 13 | Explained grid/overlay re-bless only. |

---

### Task 1: One depth-convention value

**Files:**
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/DepthConvention.hpp`
- Modify: `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCamera.hpp:18-20` (include beside the block at `:120-129`)
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/DepthConventionMap.hpp`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp:823-832`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:579-582`
- Test: `Tests/ArcaneTests/src/DepthConventionTest.cpp`

**Interfaces:**
- Consumes: `nri::CompareOp` (`ThirdParty/NRI/Include/NRIDescs.h:835-845`: `LESS`, `LESS_EQUAL`, `GREATER`, `GREATER_EQUAL`). Today's mesh compare is `CompareOp::LESS` (`MeshNode.cpp:832`). Today's pick-mesh compare is `CompareOp::LESS` (`PickOutlineNodes.cpp:582`). Today's clear is `kDepthClear = 1.0f` (`MeshNode.cpp:83`).
- Produces, exact names later tasks call:
  - `enum class DepthConvention : std::uint8_t { ForwardZ, ReverseZ };`
  - `inline constexpr DepthConvention kSelectedDepthConvention = DepthConvention::ForwardZ;`
  - `float DepthClearValue(DepthConvention convention) noexcept;`
  - `nri::CompareOp NearerWinsOp(DepthConvention convention) noexcept;`
  - `nri::CompareOp NearerOrEqualOp(DepthConvention convention) noexcept;`
  - ForwardZ returns `1.0f`, `nri::CompareOp::LESS`, `nri::CompareOp::LESS_EQUAL`. ReverseZ returns `0.0f`, `nri::CompareOp::GREATER`, `nri::CompareOp::GREATER_EQUAL`.

`DepthConvention.hpp` includes only `<cstdint>`. It does not include `Astra/Registry/Registry.hpp`, `SceneCamera.hpp`, or `NRI.h`. `SceneCamera.hpp:20` includes the registry, so a render TU that needs the enum includes `DepthConvention.hpp`, not `SceneCamera.hpp`.

- [ ] **Step 1: Write the failing test.**

Create `Tests/ArcaneTests/src/DepthConventionTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Scene/DepthConvention.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("ForwardZ clear is 1 and nearer-wins is LESS", "[nri][depth]")
{
    CHECK(Arcane::kSelectedDepthConvention == Arcane::DepthConvention::ForwardZ);
    CHECK(Arcane::DepthClearValue(Arcane::DepthConvention::ForwardZ) == 1.0f);
    CHECK(Arcane::NearerWinsOp(Arcane::DepthConvention::ForwardZ) == nri::CompareOp::LESS);
    CHECK(Arcane::NearerOrEqualOp(Arcane::DepthConvention::ForwardZ) == nri::CompareOp::LESS_EQUAL);
}

TEST_CASE("ReverseZ is the other mapping and is not selected", "[nri][depth]")
{
    CHECK(Arcane::DepthClearValue(Arcane::DepthConvention::ReverseZ) == 0.0f);
    CHECK(Arcane::NearerWinsOp(Arcane::DepthConvention::ReverseZ) == nri::CompareOp::GREATER);
    CHECK(Arcane::NearerOrEqualOp(Arcane::DepthConvention::ReverseZ) == nri::CompareOp::GREATER_EQUAL);
    CHECK(Arcane::kSelectedDepthConvention != Arcane::DepthConvention::ReverseZ);
}

static std::string ReadRepoFile(const char* relative)
{
    std::ifstream in(Arcane::Test::RepoRoot() / relative);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removing the call and writing CompareOp::LESS again goes green on the GPU
// and red here. The two sites are the mesh pipelines and the pick-mesh pipeline.
TEST_CASE("mesh and pick compares call NearerWinsOp", "[nri][depth]")
{
    const std::string mesh = ReadRepoFile("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp");
    const std::string pick = ReadRepoFile("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp");
    CHECK(mesh.find("desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);")
          != std::string::npos);
    CHECK(pick.find("desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);")
          != std::string::npos);
}
```

- [ ] **Step 2: Build and run the test to verify it fails.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[depth]"
```

Expected: FAIL. `DepthConvention.hpp` / `DepthConventionMap.hpp` do not exist, or the source scan does not find `NearerWinsOp(`. `build.ps1` builds the engine (and `arcbuild.exe`) before its ReferenceProject step, so the first build of the worktree needs nothing else. `DepthConvention.hpp` lands in ArcaneScene and `DepthConventionMap.hpp` in ArcaneRender (Global Constraints, module placement); ArcaneTests sees NRI's headers through ArcaneRender's public `thirdParty` (`Source/Client/ArcaneRender/ArcaneRender.arcmodule`).

- [ ] **Step 3: Write the headers and point the two compares at them.**

`Source/Runtime/ArcaneScene/src/Arcane/Scene/DepthConvention.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace Arcane
{
    enum class DepthConvention : std::uint8_t
    {
        ForwardZ,
        ReverseZ,
    };

    inline constexpr DepthConvention kSelectedDepthConvention = DepthConvention::ForwardZ;
}
```

In `SceneCamera.hpp`, with the other includes at file scope (`:15-20`, before `namespace Arcane` opens at `:29`), add:

```cpp
#include <Arcane/Scene/DepthConvention.hpp>
```

Not beside the comment at `:120`: that comment is inside `namespace Arcane`, and an include there would declare the enum in `Arcane::Arcane` and, through `#pragma once`, hide it from a later file-scope include in the same TU. Do not move `ActiveSceneCamera` or `ActivePerspectiveSceneCamera`. The DEPTH CONVENTION comment at `:120-129` now documents a value declared by the header included above. Leave the comment text in place so the block still documents why `perspectiveRH_ZO` is used (`SceneCamera.hpp:156`).

`Source/Client/ArcaneRender/src/Arcane/Render/DepthConventionMap.hpp`:

```cpp
#pragma once

#include <Arcane/Scene/DepthConvention.hpp>

#include <NRIDescs.h>

namespace Arcane
{
    [[nodiscard]] inline float DepthClearValue(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? 0.0f : 1.0f;
    }

    [[nodiscard]] inline nri::CompareOp NearerWinsOp(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? nri::CompareOp::GREATER
                                                       : nri::CompareOp::LESS;
    }

    [[nodiscard]] inline nri::CompareOp NearerOrEqualOp(DepthConvention convention) noexcept
    {
        return convention == DepthConvention::ReverseZ ? nri::CompareOp::GREATER_EQUAL
                                                       : nri::CompareOp::LESS_EQUAL;
    }
}
```

In `MeshNode.cpp`, include `DepthConventionMap.hpp`. Replace the assignment at line 832:

```cpp
desc.outputMerger.depth.compareOp = NearerWinsOp(kSelectedDepthConvention);
```

In `PickOutlineNodes.cpp`, the same replacement at line 582. Leave `kDepthClear` and both `CmdClearAttachments` calls for Task 2. Leave the grid's `LESS_EQUAL` (`GridNode.cpp:375`) for Task 3; Task 3's overlay reads `NearerOrEqualOp` and this task does not change grid pixels.

- [ ] **Step 4: Run the test to verify it passes.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[depth]"
```

Expected: PASS. Selected value is ForwardZ, so the mesh compare is still `LESS` and the clear constant is still `1.0f`. No golden run in this task.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Runtime/ArcaneScene/src/Arcane/Scene/DepthConvention.hpp Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCamera.hpp Source/Client/ArcaneRender/src/Arcane/Render/DepthConventionMap.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp Tests/ArcaneTests/src/DepthConventionTest.cpp
git commit -m "feat(render): one depth-convention value drives the mesh and pick compares" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 2: Declarative load ops, empty D1 slots, chrome letterbox

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraph.hpp` (`SetColorAttachments` at `:552-553`, `NodeDesc` at `:853-867`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraph.cpp` (the `SetColorAttachments` definition)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraphExec.cpp:1368-1382`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp` (`FrameDesc` at `:386`, `RgFrameShape` at `:1628`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp` (`DeclareGraphFrame` at `:1237`, `BuildFrame` copies at `:1640-1672`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1390-1408` and `:1654-1666`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp:77-83`, `:942-957`, `:1301-1320`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp:4044-4045`
- Modify: `Tests/ArcaneTests/src/RenderGraphTest.cpp` (every `DeclareGraphFrame` census listed in Step 3)
- Test: the cases added to `Tests/ArcaneTests/src/RenderGraphTest.cpp`

**Interfaces:**
- Consumes: Task 1's `DepthClearValue` and `kSelectedDepthConvention`. `nri::LoadOp` is `LOAD`, `CLEAR` (`NRIDescs.h:1552-1555`). `nri::StoreOp` zero value is `STORE` (`NRIDescs.h:1559-1562`). `nri::AttachmentDesc` is `{ descriptor, clearValue, loadOp, storeOp }` (`NRIDescs.h:1572-1578`). `NodeKind` is `Raster`, `Compute`, `Copy` (`RenderGraph.hpp:501`). `Settings<RenderSettings>().clearColor` is a `CVarColor` with `.r .g .b .a` (`CVarTypes.hpp:37`, `RenderDeviceSettings.hpp:83`).
- Produces:
  - `enum class RgLoadOp : std::uint8_t { Load, Clear };`
  - `struct RgAttachmentOp { RgLoadOp load = RgLoadOp::Load; nri::ClearValue clear{}; };`
  - `void RenderGraph::SetColorAttachmentOps(std::span<const RgAttachmentOp> ops);`
  - `void RenderGraph::SetDepthAttachmentOp(RgAttachmentOp op);`
  - `const RgAttachmentOp* RenderGraph::ColorLoadOp(std::size_t nodeIndex, std::size_t attachment) const;`
  - `const RgAttachmentOp* RenderGraph::DepthLoadOp(std::size_t nodeIndex) const;`
  - `bool FrameDesc::world = true;` and `bool RgFrameShape::world = true;`
  - Node names, in order, on a default world frame: `"gbuffer"`, `"lighting"`, `"batch2d"`, `"taa"`, `"tonemap"`.
  - Node names on `world == false` with imgui: `"letterbox"`, `"tonemap"`, `"imgui"`.

`ColorLoadOp` / `DepthLoadOp` return null when that attachment was not declared. When the colour-op span is shorter than the colour-attachment span, the missing ops are `RgLoadOp::Load` with a zero `clear` (today's behaviour). `storeOp` stays `STORE`.

- [ ] **Step 1: Write the failing tests.**

Add these cases to `Tests/ArcaneTests/src/RenderGraphTest.cpp`, and add the four includes they need to that file's include block (`RenderGraphTest.cpp:19-59` has none of them): `<Arcane/Render/DepthConventionMap.hpp>`, `"Helpers/ConstantScan.hpp"`, `<fstream>`, and `<sstream>`. They call the new queries, so they fail to compile until Step 3. `"an empty world batcher still declares batch2d"` is a regression pin rather than a failing test: once the helpers compile it passes on today's frame and must keep passing.

```cpp
TEST_CASE("batch2d colour load-op clears to render.clearColor and mesh loads colour", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 64;
    shape.canvasHeight = 64;
    Arcane::MeshInstance one;
    one.mesh = Arcane::Guid{ 1, 1 };
    const Arcane::MeshInstance instances[] = { one };
    Arcane::MeshSceneDesc scene;
    scene.instances = instances;   // span, not a vector (MeshNode.hpp:283)
    shape.mesh = &scene;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    const std::size_t batch = NodeIndex(graph, "batch2d");
    const Arcane::RgAttachmentOp* color = graph.ColorLoadOp(batch, 0);
    REQUIRE(color != nullptr);
    CHECK(color->load == Arcane::RgLoadOp::Clear);
    const Arcane::CVarColor expected = Arcane::Settings<Arcane::RenderSettings>().clearColor;
    CHECK(color->clear.color.f.x == expected.r);
    CHECK(color->clear.color.f.y == expected.g);
    CHECK(color->clear.color.f.z == expected.b);
    CHECK(color->clear.color.f.w == expected.a);

    const std::size_t mesh = NodeIndex(graph, "mesh");
    const Arcane::RgAttachmentOp* meshColor = graph.ColorLoadOp(mesh, 0);
    REQUIRE(meshColor != nullptr);
    CHECK(meshColor->load == Arcane::RgLoadOp::Load);
    const Arcane::RgAttachmentOp* meshDepth = graph.DepthLoadOp(mesh);
    REQUIRE(meshDepth != nullptr);
    CHECK(meshDepth->load == Arcane::RgLoadOp::Clear);
    CHECK(meshDepth->clear.depthStencil.depth ==
          Arcane::DepthClearValue(Arcane::kSelectedDepthConvention));

    const std::size_t gbuffer = NodeIndex(graph, "gbuffer");
    CHECK(graph.ColorLoadOp(gbuffer, 0) == nullptr);
    CHECK(graph.DepthLoadOp(gbuffer) == nullptr);
}

TEST_CASE("reserved D1 slots are empty and sit around the world nodes", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 64;
    shape.canvasHeight = 64;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    const std::vector<std::string> names = NodeNames(graph);
    CHECK(names == std::vector<std::string>{ "gbuffer", "lighting", "batch2d", "taa", "tonemap" });

    for (const char* empty : { "gbuffer", "lighting", "taa" })
    {
        const std::size_t i = NodeIndex(graph, empty);
        CHECK(graph.ColorLoadOp(i, 0) == nullptr);
        CHECK(graph.DepthLoadOp(i) == nullptr);
    }
}

// Review Focus 1.
TEST_CASE("chrome world=false is letterbox, tonemap, imgui", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 1280;
    shape.canvasHeight = 720;
    shape.imgui        = true;
    shape.world        = false;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    CHECK(NodeNames(graph) == std::vector<std::string>{ "letterbox", "tonemap", "imgui" });
    const std::size_t letter = NodeIndex(graph, "letterbox");
    const Arcane::RgAttachmentOp* clear = graph.ColorLoadOp(letter, 0);
    REQUIRE(clear != nullptr);
    CHECK(clear->load == Arcane::RgLoadOp::Clear);
    const Arcane::CVarColor expected = Arcane::Settings<Arcane::RenderSettings>().clearColor;
    CHECK(clear->clear.color.f.x == expected.r);

    for (const char* banned : { "batch2d", "mesh", "grid", "overlay", "gbuffer",
                                "lighting", "taa", "gpuscene-sync", "mesh-cull" })
        CHECK(NodeIndexOrNpos(graph, banned) == static_cast<std::size_t>(-1));
}

TEST_CASE("an empty world batcher still declares batch2d", "[nri][loadop]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 32;
    shape.canvasHeight = 32;
    // world defaults true. No batcher pointer on the shape: declaration only.
    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(NodeIndexOrNpos(graph, "batch2d") != static_cast<std::size_t>(-1));
    CHECK(NodeIndexOrNpos(graph, "letterbox") == static_cast<std::size_t>(-1));
}

TEST_CASE("PresentChromeFrame sets frame.world = false", "[nri][loadop]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t fn = text.find("EditorApp::PresentChromeFrame()");
    REQUIRE(fn != std::string::npos);
    // Member definitions are indented four spaces inside `namespace Arcane::Editor`
    // (EditorAppFrame.cpp:4026), so the body ends at the first four-space `}` line.
    const std::size_t next = text.find("\n    }\n", fn + 1);
    REQUIRE(next != std::string::npos);
    const std::string body = text.substr(fn, next - fn);
    CHECK(body.find("frame.world = false;") != std::string::npos);
}

TEST_CASE("batch2d and mesh Record no longer call CmdClearAttachments", "[nri][loadop]")
{
    auto read = [](const char* rel) {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    auto recordBody = [](const std::string& text, const char* sig) {
        const std::size_t fn = text.find(sig);
        REQUIRE(fn != std::string::npos);
        return text.substr(fn, 2500);
    };
    const std::string batch = recordBody(
        read("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp"),
        "void Batch2DNode::Record(");
    const std::string mesh = recordBody(
        read("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp"),
        "void MeshNode::Record(");
    CHECK(batch.find("CmdClearAttachments") == std::string::npos);
    CHECK(mesh.find("CmdClearAttachments") == std::string::npos);
    // Pick keeps its own clear. This scan is the guard that the deletion stayed local.
    const std::string pick = read("Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp");
    CHECK(pick.find("CmdClearAttachments") != std::string::npos);
}
```

Add the helpers next to the other anonymous helpers in that file:

```cpp
std::vector<std::string> NodeNames(const Arcane::RenderGraph& graph)
{
    std::vector<std::string> names;
    names.reserve(graph.NodeCount());
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
        names.emplace_back(graph.NodeName(i));
    return names;
}

std::size_t NodeIndexOrNpos(const Arcane::RenderGraph& graph, const char* name)
{
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
        if (std::string(graph.NodeName(i)) == name)
            return i;
    return static_cast<std::size_t>(-1);
}

std::size_t NodeIndex(const Arcane::RenderGraph& graph, const char* name)
{
    const std::size_t i = NodeIndexOrNpos(graph, name);
    REQUIRE(i != static_cast<std::size_t>(-1));
    return i;
}

// Today's world-frame name list, with the three empty slots inserted.
// Chrome does not use this helper: chrome sets world=false and expects letterbox.
std::vector<std::string> WithReservedSlots(std::vector<std::string> today)
{
    std::vector<std::string> out{ "gbuffer", "lighting" };
    for (const std::string& name : today)
    {
        if (name == "tonemap")
            out.emplace_back("taa");
        out.push_back(name);
    }
    return out;
}
```

`MeshSceneDesc::instances` is `std::span<const MeshInstance>` (`MeshNode.hpp:283`). The one-element array above is the fixture at `RenderGraphTest.cpp:6707-6712`. `wantsMesh` is `shape.mesh != nullptr && !shape.mesh->Empty()` (`NriGraphContext.cpp:1309`). `Empty()` is false when `instances` is non-empty (`MeshNode.hpp:352-355`).

- [ ] **Step 2: Run the new cases to verify they fail.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[loadop]"
```

Expected: FAIL to compile (`RgLoadOp`, `ColorLoadOp`, `shape.world` unknown) or, once those exist and before the behaviour lands, FAIL the name and source checks.

- [ ] **Step 3: Implement the load ops, the three empty nodes, and the letterbox.**

On `RenderGraph::NodeDesc` (`RenderGraph.hpp:853`), add:

```cpp
std::vector<RgAttachmentOp> colorOps;
RgAttachmentOp              depthOp{};
bool                        hasDepthOp = false;
```

Declare `RgLoadOp` and `RgAttachmentOp` in namespace `Arcane` in `RenderGraph.hpp` above `RenderGraph`. Add the two setters next to `SetColorAttachments` (`:552`) and the two queries next to `NodeName` (`:690`).

`SetColorAttachmentOps` stores the span into `m_nodes[m_currentNodeIndex].colorOps`, under the same "only while setup is running" assert `SetColorAttachments` already uses. `SetDepthAttachmentOp` stores `op` and sets `hasDepthOp`. `ColorLoadOp` returns null when `nodeIndex` or `attachment` is out of range, or when `colorOps.size() <= attachment` (shorter span means LOAD, reported as null so a caller that did not set an op does not observe a synthetic CLEAR). `DepthLoadOp` returns null when `!hasDepthOp`.

In `RenderGraphExec.cpp`, replace the colour-attachment loop at `:1368-1373`:

```cpp
m_scratchColorAttachments.clear();
std::size_t colorIndex = 0;
for (const RgTexture& color : node.colorAttachments)
{
    nri::AttachmentDesc attachment = {};
    attachment.descriptor = attachmentView(color, /*depth=*/false);
    if (colorIndex < node.colorOps.size() && node.colorOps[colorIndex].load == RgLoadOp::Clear)
    {
        attachment.loadOp = nri::LoadOp::CLEAR;
        attachment.clearValue = node.colorOps[colorIndex].clear;
    }
    m_scratchColorAttachments.push_back(attachment);
    ++colorIndex;
}
```

After the existing depth-descriptor assignment (`:1379-1380`), add:

```cpp
if (node.hasDepthOp && node.depthOp.load == RgLoadOp::Clear)
{
    renderingDesc.depth.loadOp = nri::LoadOp::CLEAR;
    renderingDesc.depth.clearValue = node.depthOp.clear;
}
```

Do not set `storeOp`. Zero-initialization already stores `STORE`.

`AddBatch2DNode` (`Batch2DNode.cpp:1654`) already writes `optimizedClearValue` from `clearColor` (`:1661-1662`). Inside that setup lambda, after `SetColorAttachments`, add:

```cpp
Arcane::RgAttachmentOp op;
op.load = Arcane::RgLoadOp::Clear;
op.clear.color.f = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
graph.SetColorAttachmentOps(std::span<const Arcane::RgAttachmentOp>(&op, 1));
```

Delete the `CmdClearAttachments` block in `Batch2DNode::Record` (`:1395-1406`: the comment that says the declaration API carries no clear op, the `ClearAttachmentDesc`, and the call at `:1405`), and nothing after it. Keep the empty-batch early return at `:1407-1408`, which guards the upload-ring allocation at `:1427-1431`: an empty batch still returns after the pass has begun, and the load-op is what cleared the target. The early return stays so an empty batch records no draws.

`MeshNode::AddMeshNode` setup (`:1319-1320` region) after the colour and depth attachments:

```cpp
Arcane::RgAttachmentOp colorOp;   // load defaults to Load
graph.SetColorAttachmentOps(std::span<const Arcane::RgAttachmentOp>(&colorOp, 1));
Arcane::RgAttachmentOp depthOp;
depthOp.load = Arcane::RgLoadOp::Clear;
depthOp.clear.depthStencil.depth = Arcane::DepthClearValue(Arcane::kSelectedDepthConvention);
depthOp.clear.depthStencil.stencil = 0;
graph.SetDepthAttachmentOp(depthOp);
```

Replace `kDepthClear` (`MeshNode.cpp:83`) and its uses at the `optimizedClearValue` write (`:1309`) with `DepthClearValue(kSelectedDepthConvention)`. Delete the depth `CmdClearAttachments` block (`:942-957`). Do not delete pick's clear.

In `DeclareGraphFrame`, when `shape.world` is true, before `AddBatch2DNode` (`:1283`), add two compute nodes:

```cpp
auto addEmpty = [&](const char* name) {
    graph.AddNode(name, RenderGraph::NodeKind::Compute,
        [](RenderGraphBuilder&) {},
        [](RenderGraphNodeContext&) {});
};
addEmpty("gbuffer");
addEmpty("lighting");
```

Immediately before `AddTonemapNode` (`:1451`), `addEmpty("taa")`. These nodes declare no `Read`, no `Write`, and no attachment, so `Compile` gives them no barrier and no pool slot. They do not set a colour clear.

When `shape.world` is false, do not call `addEmpty`, do not call `AddBatch2DNode`, and do not run the mesh, depth-placeholder, grid, or post blocks. Declare one raster node `"letterbox"` whose setup is `AddBatch2DNode`'s (`Batch2DNode.cpp:1654-1666`) minus the batch: `RgTextureDesc` with `GraphCanvasFormat()` and `shape.canvasWidth/Height`, `optimizedClearValue` from `Settings<RenderSettings>().clearColor` with `hasOptimizedClearValue = true`, `CreateTexture("canvas", desc)`, `Write(canvas, RgUsage::ColorWrite)`, `SetColorAttachments` of that one texture, then `SetColorAttachmentOps` of a `Clear` op to the same colour. The exec is empty. `SetColorAttachments` is required: `Compile` refuses a raster node with no colour or depth attachment (`RenderGraph.hpp:564-569`), and the chrome census at `RenderGraphTest.cpp:6243` compiles this frame. Store that texture in `handles.canvas` so `AddTonemapNode` still samples it. Then declare tonemap, then the existing gameui / pick / imgui / capture tail. Capture still appends when `shape.capture` is set. Chrome does not set capture.

`BuildFrame` (`:1641`) copies `shape.world = frame.world`. Default `true` means every current caller stays a world frame until it opts out.

`PresentChromeFrame` (`EditorAppFrame.cpp:4044-4045`), immediately after `FrameDesc frame;`:

```cpp
frame.world = false;
frame.imgui = chrome;
```

**Existing `DeclareGraphFrame` censuses.** Any test that `AddNode`s its own names (`"first"`, `"second"`) is unchanged. Every test that calls `DeclareGraphFrame` on a default (`world == true`) shape changes its expected names by `WithReservedSlots` of today's list, and its `NodeCount` by `+3` (two prefix nodes and one `taa`). Hard indices into `compiled.nodes[i]` that assumed node 0 was `batch2d` retarget through `NodeIndex(graph, "batch2d")`. `compiled.nodes[tonemap]` retargets through `NodeIndex(graph, "tonemap")`. Transient counts and pool-slot counts stay, because the three new nodes allocate nothing.

The index rule: a node declared before `tonemap` moves up by 2 (the two prefix nodes); `tonemap` and every node after it moves up by 3 (the prefix plus `taa`). `NodeCount()` and `compiled.nodes.size()` grow by 3. An index written relative to the end (`NodeCount() - k`) does not change. Transient counts, pool-slot counts, and `kRgNoNode` lifetimes do not change. Apply it at every index-bearing assertion of every `DeclareGraphFrame` case in `RenderGraphTest.cpp` (every call site re-read in this tree; the rows below are every one with an index, count, or name assertion):

| Site | Today | After this task |
|---|---|---|
| `:3239` | count 2 | count 5 |
| `:4135-4166` | count 2; `[0]` batch2d, `[1]` tonemap; `nodes.size()` 2; `nodes[0]` canvas barrier, `nodes[1]` tonemap barriers | count 5: gbuffer, lighting, batch2d, taa, tonemap; size 5; `nodes[2]`, `nodes[4]` |
| `:4212-4230` | count 3; `NodeName(2)` capture; size 3; `nodes[2]` | count 6; `NodeName(5)`; size 6; `nodes[5]` |
| `:4313-4338` | count 2; batch2d, tonemap; size 2; `nodes[1]` | count 5; the five; size 5; `nodes[4]` |
| `:4420` | offscreen vs presenting count equality | both sides gain the same three; equality stays |
| `:4461-4471` | count 3; `NodeName(2)` capture; size 3; `nodes[2]` | count 6; `NodeName(5)`; size 6; `nodes[5]` |
| `:4577` | `second.nodes.size() == first.nodes.size()` | stays |
| `:4656-4710` | count 4: batch2d, post0, post1, tonemap; size 4; `nodes[1]`, `[2]`, `[3]` | count 7: gbuffer, lighting, batch2d, post0, post1, taa, tonemap; size 7; `nodes[3]`, `[4]`, `[6]` |
| `:4740-4741` | `expected.nodes` 2 and 4 | 5 and 7 |
| `:4767` | count 2 | count 5 |
| `:4789`, `:4826` | count 6 (batch2d + four posts + tonemap); `nodes[1]` | count 9; `nodes[3]` |
| `:4849`, `:4875`, `:4881-4882` | count 4; count equality; `nodes[2]` | count 7; equality stays; `nodes[4]` |
| `:5787-5796` | count 2; batch2d, tonemap; size 2 | count 5; the five; size 5 |
| `:5822-5833` | count `6u + steps`; `[0]` batch2d, `[1]` tonemap, `[2]` pick, `[3]` pickreadback, `[4]` outlineseed, `[5 + step]` outlinejfa, `[5 + steps]` outlinecomposite | count `9u + steps`; `[2]`, `[4]`, `[5]`, `[6]`, `[7]`, `[8 + step]`, `[8 + steps]` |
| `:5873-5976` | `nodes.size() == 6u + steps`; `nodes[2]` (pick), `[3]`, `[4]`, `[5 + step]`, `[5 + steps]` | `9u + steps`; `nodes[5]`, `[6]`, `[7]`, `[8 + step]`, `[8 + steps]` |
| `:6012`, `:6028` | `poolSlotCount == 5` | stays |
| `:6049-6054` | count `7u + steps`; `NodeName(5 + steps)` outlinecomposite, `NodeName(6 + steps)` capture; `nodes[6 + steps]` | count `10u + steps`; `NodeName(8 + steps)`, `NodeName(9 + steps)`; `nodes[9 + steps]` |
| `:6099-6102` | count `9u + jfaStepCount`; `NodeName(4)` tonemap, `NodeName(5)` pick; last is outlinecomposite | count `12u + jfaStepCount`; `NodeName(7)`, `NodeName(8)`; last stays |
| `:6126-6131` | count 2; batch2d, tonemap; size 2 | count 5; the five; size 5 |
| `:6149-6168` | count 3; `NodeName(1)` tonemap, `NodeName(2)` imgui; size 3; `nodes[2]` | count 6; `NodeName(4)`, `NodeName(5)`; size 6; `nodes[5]` |
| `:6177-6255` | batch2d, tonemap, imgui; size 3; `nodes[2]`; one transient, one pool slot | **set `shape.world = false`**. Expect letterbox, tonemap, imgui: count 3, size 3, `nodes[2]`, one transient, one pool slot. This is the chrome case; do not run it through `WithReservedSlots`. |
| `:6278-6292` | count 4; `NodeName(2)` imgui, `NodeName(3)` capture; size 4; `nodes[2]`, `nodes[3]` | count 7; `NodeName(5)`, `NodeName(6)`; size 7; `nodes[5]`, `nodes[6]` |
| `:6310-6311` | `expected.nodes` 2 and 3 | 5 and 6 |
| `:6356-6370` | count `10u + steps`; `NodeName(3)` tonemap; the end-relative names and `nodes[NodeCount() - 2]` | count `13u + steps`; `NodeName(6)`; end-relative unchanged |
| `:6400-6410` | count 3; `NodeName(1)` tonemap, `NodeName(2)` gameui; size 3; `nodes[2]` | count 6; `NodeName(4)`, `NodeName(5)`; size 6; `nodes[5]` |
| `:6427-6431` | count `7u + steps`; `NodeName(1)` tonemap, `(2)` gameui, `(3)` pick | count `10u + steps`; `(4)`, `(5)`, `(6)` |
| `:6454-6464` | count `9u + steps`; `NodeName(2)` gameui; `nodes[2]` | count `12u + steps`; `NodeName(5)`; `nodes[5]` |
| `:6494-6495`, `:6505-6506` | count 2, `NodeName(1)` tonemap; count 3, `NodeName(2)` gameui | count 5, `NodeName(4)`; count 6, `NodeName(5)` |
| `:6538-6544` | count 2; batch2d, tonemap; size 2 | count 5; the five; size 5 |
| `:6562-6585` | count 3: batch2d, depth, tonemap; size 3 | count 6: gbuffer, lighting, batch2d, depth, taa, tonemap; size 6; the `kRgNoNode` lifetimes stay |
| `:6617-6619` | `nodes.size() == 4`; `nodes[3]` is `test-depth-consumer` (added after `DeclareGraphFrame`) | size 7; `nodes[6]` |
| `:6663-6665` | count 2; batch2d, tonemap | count 5; the five |
| `:6683-6686` | count 3: batch2d, depth, tonemap | count 6: gbuffer, lighting, batch2d, depth, taa, tonemap |
| `:6722-6750` | count 5: batch2d, gpuscene-sync, mesh-cull, mesh, tonemap; size 5; `nodes[3]` (mesh) | count 8: gbuffer, lighting, batch2d, gpuscene-sync, mesh-cull, mesh, taa, tonemap; size 8; `nodes[5]` |
| `:6810-6822`, `:6900-6959` | `AddMeshNode` driven directly (`:6806`, `:6893`), count 4 | **unchanged**: these cases do not call `DeclareGraphFrame` |
| `:7007-7012` | count 5 (mesh chain) | count 8, names as `:6722` |
| `:7054-7056` | count 2 | count 5 |
| `:7101-7136` | count 6 with `grid` at 4; size 6; depth lifetime `first == 3`, `last == 4`; `nodes[4].preBarriers.empty()` | count 9: gbuffer, lighting, batch2d, gpuscene-sync, mesh-cull, mesh, grid, taa, tonemap; size 9; `first == 5`, `last == 6`; `nodes[6]`. Task 3 rewrites this section. |
| `:7149-7160` | count 3: batch2d, grid, tonemap; size 3; `nodes[1]` | count 6: gbuffer, lighting, batch2d, grid, taa, tonemap; size 6; `nodes[3]`. Task 3 rewrites this section. |
| `:7172-7176` | count 5 | count 8; `NodeName(3)` gpuscene-sync, `(4)` mesh-cull, `(5)` mesh, `(7)` tonemap |
| `:7211-7218` | direct `AddGridNode` | unchanged; it does not call `DeclareGraphFrame` |
| `:7594-7604` | count 2; `NodeName(0)` batch2d; `nodes[0].preBarriers.size() == 1` | count 5; `NodeName(2)`; `nodes[2]` (node 0 is now `gbuffer`, which declares no access) |

Prefer `NodeIndex(graph, "<name>")` over a literal where the case already names the node; the literals above are what the rule produces.

Do not run the golden gate. World pixels are unchanged: `batch2d` still precedes `mesh`, the clear value is the same `clearColor`, the depth clear is still `1.0`, the compare is still `LESS`. The grid node is still before the post chain.

- [ ] **Step 4: Run the graph tests.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[nri]"
```

Expected: PASS, including `[loadop]` and the updated censuses. Record the seed.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraph.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraph.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderGraphExec.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/MeshNode.cpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/RenderGraphTest.cpp
git commit -m "feat(render): declarative load ops, empty D1 slots, chrome letterbox" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 3: Post-tonemap overlay pass

**Files:**
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/GridSceneDesc.hpp`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/GridNode.hpp` (replace the struct at `:96-156` with the include)
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/OverlayPrimitive.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.cpp`
- Create: `data/shaders/tonemap_operator.hlsl`
- Modify: `data/shaders/tonemap.hlsl:36-50`
- Create: `data/shaders/overlay.hlsl`
- Modify: `data/shaders/compile-shaders.bat:68-69` (add the `call :compile` lines after the grid lines; `compile-shaders.sh` reads them from the `.bat`, `compile-shaders.sh:7-10`, so it is not edited)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp` (`RgFrameShape` grid field at `:1742`, `FrameDesc::grid` at `:452`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp` (delete the `AddGridNode` call at `:1398-1403`; declare `"overlay"` after tonemap at `:1451` and before gameui at `:1480`)
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/OverlayBatchCollector.hpp` (header-only `Batcher2D` subclass that records `Line`/`Circle`/`Rect` as overlay primitives; no export macro)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` (`SubmitSceneToBatcher` `:1866-2001`, gizmo lambda inside `DrawViewportPanelPhase` at `:3723`, `MainLoop` at `:479-480`, `ArmGraphViewportFrame` `:2052`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorApp.hpp` (members `m_overlayPrims` and `m_gizmoPrims`, method `BuildGizmoOverlayPrims`)
- Modify: `Tests/ArcaneTests/src/RenderGraphTest.cpp` grid frame-shape case at `:7077-7178` (all three sections)
- Test: `Tests/ArcaneTests/src/OverlayWitnessTest.cpp`

**Interfaces:**
- Consumes: `NearerOrEqualOp`, `kSelectedDepthConvention`, `RgFrameShape::world`, node `"tonemap"`, `handles.backbuffer`, `handles.depth`. `GridSceneDesc` fields are the struct at `GridNode.hpp:96-156` (`view`, `plane`, spacings, `minorColor`, `majorColor`, axis colours, `SetPlane`). `ImGuiGizmoSink` methods are `Line`, `Triangle`, `Rect`, `Circle` (`GizmoOverlay.hpp:26-41`), colours already display-referred.
- Produces:
  - `bool RenderGraph::WasRead(RgTexture texture) const;` — true when some node `Read`s that texture. There is no such query today (`WasWritten` is `RenderGraph.cpp:892`).
  - `enum class OverlayDepthClass : std::uint8_t { DepthTested, AlwaysOnTop };`
  - `enum class OverlayColorSpace : std::uint8_t { Linear, DisplayReferred };`
  - `enum class OverlayPrimKind : std::uint8_t { Line, Triangle, Rect, Circle };`
  - `struct OverlayLine { glm::vec3 a; glm::vec3 b; glm::vec4 color; float thickness; OverlayDepthClass depth; OverlayColorSpace space; bool screenSpace; };`
  - `struct OverlayFill { OverlayPrimKind kind; glm::vec3 a, b, c; glm::vec2 size; float radius; glm::vec4 color; OverlayDepthClass depth; OverlayColorSpace space; bool screenSpace; };`
  - `struct OverlayPrimitiveList { std::vector<OverlayLine> lines; std::vector<OverlayFill> fills; bool hasGrid3d = false; GridSceneDesc grid3d{}; };`
  - `const OverlayPrimitiveList* FrameDesc::overlay = nullptr;`
  - `const OverlayPrimitiveList* RgFrameShape::overlay = nullptr;`
  - Node name `"overlay"`, declared after `"tonemap"` and before `"gameui"`, only when `shape.grid != nullptr` or `shape.overlay` has any line, fill, or `hasGrid3d`.

`GridSceneDesc.hpp` includes `ViewTransform.hpp`, `Constant.hpp` (for the existing `ARC_CONSTANT` markers), and `<glm/glm.hpp>`. It does not include `NRI.h` or `Registry.hpp`. `GridNode.hpp` includes `GridSceneDesc.hpp` and deletes the struct body.

- [ ] **Step 1: Write the failing tests.**

`Tests/ArcaneTests/src/OverlayWitnessTest.cpp`. The device-less half fails at compile time until the types exist. The GPU half fails until the pass behaves.

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/DepthConventionMap.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <sstream>

static std::size_t NodeIndexOrNpos(const Arcane::RenderGraph& graph, const char* name)
{
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
        if (std::string(graph.NodeName(i)) == name) return i;
    return static_cast<std::size_t>(-1);
}
static std::size_t NodeIndex(const Arcane::RenderGraph& graph, const char* name)
{
    const std::size_t i = NodeIndexOrNpos(graph, name);
    REQUIRE(i != static_cast<std::size_t>(-1));
    return i;
}

TEST_CASE("overlay node sits after tonemap and samples mesh depth", "[nri][overlay]")
{
    Arcane::MeshInstance one;
    one.mesh = Arcane::Guid{ 1, 1 };
    const Arcane::MeshInstance instances[] = { one };
    Arcane::MeshSceneDesc scene;
    scene.instances = instances;

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    shape.mesh = &scene;
    Arcane::OverlayPrimitiveList list;
    Arcane::OverlayLine line{};
    line.a = { 0.0f, 0.0f, 0.0f };
    line.b = { 1.0f, 0.0f, 0.0f };
    line.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    line.depth = Arcane::OverlayDepthClass::DepthTested;
    line.space = Arcane::OverlayColorSpace::Linear;
    list.lines.push_back(line);
    shape.overlay = &list;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::size_t tonemap = NodeIndex(graph, "tonemap");
    const std::size_t overlay = NodeIndex(graph, "overlay");
    CHECK(overlay == tonemap + 1);
    CHECK(NodeIndexOrNpos(graph, "grid") == static_cast<std::size_t>(-1));
    CHECK(graph.DepthLoadOp(overlay) == nullptr);   // sampled, not attached
    REQUIRE(graph.IsHandleValid(handles.depth));
    CHECK(graph.WasRead(handles.depth));
    CHECK(graph.WasWritten(handles.depth));         // the mesh node still writes it
}

TEST_CASE("a runtime frame with no overlay does not declare one", "[nri][overlay]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(NodeIndexOrNpos(graph, "overlay") == static_cast<std::size_t>(-1));
    CHECK(NodeIndexOrNpos(graph, "grid") == static_cast<std::size_t>(-1));
}

TEST_CASE("overlay.hlsl uses the shared operator and has no exposure term", "[nri][overlay]")
{
    auto read = [](const char* rel) {
        std::ifstream in(Arcane::Test::RepoRoot() / rel);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string overlay = read("data/shaders/overlay.hlsl");
    const std::string tonemap = read("data/shaders/tonemap.hlsl");
    const std::string shared  = read("data/shaders/tonemap_operator.hlsl");
    CHECK(overlay.find("exposure") == std::string::npos);
    CHECK(overlay.find("#include \"tonemap_operator.hlsl\"") != std::string::npos);
    CHECK(tonemap.find("#include \"tonemap_operator.hlsl\"") != std::string::npos);
    CHECK(shared.find("float3 ACESFilmic(float3 x)") != std::string::npos);
    CHECK(shared.find("float3 LinearToSrgb(float3 lin)") != std::string::npos);
    // The constants stay the Narkowicz values. A drift here moves world pixels.
    CHECK(shared.find("const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;")
          != std::string::npos);
    // Screen-space clip.z is the vertex z. A hardcoded 0 is the near plane, so a
    // depth-tested overlay could never sit behind a mesh.
    CHECK(overlay.find("float4(ndc, input.pos.z, 1.0)") != std::string::npos);
    CHECK(overlay.find("float4(ndc, 0.0, 1.0)") == std::string::npos);
    CHECK(overlay.find("g_HasDepth > 0.5 && input.depthTest > 0.5") != std::string::npos);
    // SV_Position.z is already post-divide. A second divide by .w must fail this scan.
    CHECK(overlay.find("input.clip.z / input.clip.w") == std::string::npos);
    CHECK(overlay.find("clip.z / clip.w") == std::string::npos);
    const auto fragD = overlay.find("float fragD = input.clip.z;");
    CHECK(fragD != std::string::npos);
    CHECK(overlay.find("float fragD = input.clip.z;", fragD + 1) != std::string::npos);
}

// The placeholder depth node (NriGraphContext.cpp:1358-1370) stays unconsumed.
// Reading it would make WasRead true. Today's placeholder case already requires
// WasWritten(depth) == false (RenderGraphTest.cpp:6673-6695).
TEST_CASE("overlay with no mesh does not read the placeholder depth", "[nri][overlay]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth = 64;
    shape.canvasHeight = 64;
    shape.depth = true;
    Arcane::OverlayPrimitiveList list;
    Arcane::OverlayLine line{};
    line.a = { 4.0f, 10.0f, 1.0f };
    line.b = { 16.0f, 10.0f, 1.0f };
    line.thickness = 8.0f;
    line.depth = Arcane::OverlayDepthClass::DepthTested;
    line.space = Arcane::OverlayColorSpace::DisplayReferred;
    line.color = { 0.0f, 0.0f, 1.0f, 1.0f };
    line.screenSpace = true;
    list.lines.push_back(line);
    shape.overlay = &list;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::size_t tonemap = NodeIndex(graph, "tonemap");
    const std::size_t overlay = NodeIndex(graph, "overlay");
    CHECK(overlay == tonemap + 1);
    CHECK(NodeIndexOrNpos(graph, "depth") != static_cast<std::size_t>(-1));
    REQUIRE(graph.IsHandleValid(handles.depth));
    CHECK_FALSE(graph.WasRead(handles.depth));
    CHECK_FALSE(graph.WasWritten(handles.depth));
}

// Review Focus 3, CPU half. The operator is nonlinear, so the two composites differ
// even when the post chain is empty. This fails if someone "proves" identity by
// algebra that assumes a linear tonemap.
TEST_CASE("tonemap-then-blend differs from blend-then-tonemap", "[nri][overlay]")
{
    const glm::vec3 scene{ 0.02f, 0.02f, 0.04f };
    const glm::vec3 prim{ 0.8f, 0.8f, 0.2f };
    const float a = 0.5f;
    const glm::vec3 blendThen = TonemapOperator(prim * a + scene * (1.0f - a));
    const glm::vec3 tonemapThen = TonemapOperator(prim) * a + TonemapOperator(scene) * (1.0f - a);
    CHECK(blendThen != tonemapThen);
}
```

That case pins the numeric claim only (the two orders differ, including with an empty chain); it touches no engine code and passes before `overlay.hlsl` exists. The shader is pinned by the `overlay.hlsl` source scan above and by the GPU cases below. `TonemapOperator` is a C++ copy of the HLSL, in the test file, with the same constants:

```cpp
glm::vec3 AcesFilmic(glm::vec3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    glm::vec3 y = (x * (a * x + b)) / (x * (c * x + d) + e);
    return glm::clamp(y, glm::vec3(0.0f), glm::vec3(1.0f));
}
float LinearToSrgb1(float lin)
{
    return std::min(lin * 12.92f, std::pow(std::max(lin, 0.0031308f), 1.0f / 2.4f) * 1.055f - 0.055f);
}
glm::vec3 TonemapOperator(glm::vec3 linear)
{
    const glm::vec3 mapped = AcesFilmic(linear);
    return { LinearToSrgb1(mapped.x), LinearToSrgb1(mapped.y), LinearToSrgb1(mapped.z) };
}
```

`WasRead` is added in Step 3. The runtime-frame case below uses the same `NodeIndexOrNpos`.

GPU cases live in the same file. The helpers they call are in `NriGraphPixelTest.cpp`'s anonymous namespace and are not linkable from here. Paste these ranges into this file's anonymous namespace, unchanged:

- `kW`, `kH`, `PixelVehicle`, `MakeVehicle`, `Rgba`, `At`, `Luma`, `RenderOne` — `NriGraphPixelTest.cpp:140-235`. `kW` is 160 and `kH` is 96. `MakeVehicle` takes `(GraphicsBackend, const NodeSet& = {})`. The default `NodeSet` (`NriGraphContext.hpp:360-379`) already builds the mesh node; there is no mesh flag to set.
- `kEyeZ`, `kFovYDegrees`, `kNearZ`, `kFarZ`, `kAmbient`, `FillCamera`, and `SupplyOne` — `NriGraphPixelTest.cpp:880-927` (`FillCamera` reads the five constants at `:880-888`).
- `struct GpuSceneWorld` — `NriGraphPixelTest.cpp:2150-2188`.

Add the includes those ranges use, the same ones at the top of `NriGraphPixelTest.cpp` (`HostConfig.hpp`, `GpuSceneSync.hpp`, `VisibilitySystem.hpp`, `BoundsSystem.hpp`, `Components.hpp`, `SceneModule.hpp`, `SceneResources.hpp`, `TransformSystems.hpp`, `SceneCamera.hpp`, `MeshBuilder.hpp`, `RenderErrorLatch.hpp`, `NriDevice.hpp`, `GpuScene.hpp`, `NriMeshBufferCache.hpp`, `Helpers/GpuCapability.hpp`, `Astra/Registry/Registry.hpp`, `glm/gtc/matrix_transform.hpp`), plus `PostChainCache.hpp`, `ShaderCompiler.hpp`, `ShaderSourceProvider.hpp`, `ShaderConventions.hpp`, `MaterialSource.hpp`, and `MaterialInstance.hpp`.

`BuildGpuSceneFrame` in this task still takes `const MeshTable*` (`GpuSceneSync.hpp:285`). Task 6 changes that signature and updates the call below in the same commit as the other call sites.

A screen-space line is expanded in Step 3 into a quad of pixel `thickness`. The segments below are 16px long and 8px thick, so the named pixel centre sits inside the quad: `(80, 48)` is the centre of the 160x96 target (`NriGraphPixelTest.cpp:2250` reads that centre as the lit cube), and `(10, 10)` is the background corner that same capture reads.

```cpp
Arcane::OverlayLine ScreenLine(float x0, float y0, float x1, float y1, float z,
                               Arcane::OverlayDepthClass depth,
                               Arcane::OverlayColorSpace space,
                               glm::vec4 color)
{
    Arcane::OverlayLine line{};
    line.a = { x0, y0, z };
    line.b = { x1, y1, z };
    line.thickness = 8.0f;
    line.depth = depth;
    line.space = space;
    line.color = color;
    line.screenSpace = true;
    return line;
}

void CheckOverlayClasses(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    // The cube sequence at NriGraphPixelTest.cpp:2202-2239. Centre pixel is the
    // lit red cube; pixel (10, 10) is background.
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    GpuSceneWorld w;
    w.AddMesh(cubeId, cube);
    w.Spawn(glm::vec3(0.0f), cubeId);
    w.Schedulers();

    Arcane::MeshSceneDesc scene;
    FillCamera(scene);
    Arcane::ViewTransform view;
    view.view       = scene.view;
    view.projection = scene.projection;
    view.viewport   = glm::uvec2{ kW, kH };

    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, view, vis);
    Arcane::GpuSceneMirror mirror;
    Arcane::GpuSceneFrame gpu;
    Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
    Arcane::BuildGpuSceneFrame(mirror, &vis, w.reg.GetResource<Arcane::MeshTable>(), view, gpu);
    REQUIRE(gpu.stage.rows.size() == 1u);
    for (Arcane::GpuInstance& row : gpu.stage.values)
        row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list, bool withMesh) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        if (withMesh)
            frame.mesh = &scene;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    // pos.z = 1 is the far plane (clip.w is 1). The cube's front face is nearer
    // (eye at z = 4, BuildCube(2) half-extent 1, near 0.1, far 100). Forward-Z
    // discards when fragD > sceneD, so this green line is hidden. A shader that
    // writes clip.z = 0 puts the line on the near plane and the centre goes green.
    Arcane::OverlayPrimitiveList hidden;
    hidden.lines.push_back(ScreenLine(72.0f, 48.0f, 88.0f, 48.0f, 1.0f,
        Arcane::OverlayDepthClass::DepthTested,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));
    const std::vector<unsigned char> covered = shoot(hidden, true);
    const Rgba cubePixel = At(covered, kW, kW / 2u, kH / 2u);
    CHECK(cubePixel.r > cubePixel.g + 60);

    Arcane::OverlayPrimitiveList onTop = hidden;
    onTop.lines[0].depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    const std::vector<unsigned char> top = shoot(onTop, true);
    const Rgba topPixel = At(top, kW, kW / 2u, kH / 2u);
    CHECK(topPixel.r == 0);
    CHECK(topPixel.g == 255);
    CHECK(topPixel.b == 0);

    // Display-referred (1,0,0) is not ACES(1,0,0). ACES(1) is about 0.8038,
    // which encodes to a byte near 231, not 255. Pixel (10, 10) is off the cube.
    Arcane::OverlayPrimitiveList red;
    red.lines.push_back(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 0.0f,
        Arcane::OverlayDepthClass::AlwaysOnTop,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
    const std::vector<unsigned char> shown = shoot(red, true);
    const Rgba shownPixel = At(shown, kW, 10u, 10u);
    CHECK(shownPixel.r == 255);
    CHECK(shownPixel.g == 0);
    CHECK(shownPixel.b == 0);

    Arcane::OverlayPrimitiveList graded = red;
    graded.lines[0].space = Arcane::OverlayColorSpace::Linear;
    const std::vector<unsigned char> gradedPx = shoot(graded, true);
    const Rgba gradedPixel = At(gradedPx, kW, 10u, 10u);
    CHECK(gradedPixel.r > 200);
    CHECK(gradedPixel.r < 250);

    // No mesh: g_HasDepth is 0, so z = 1 is still drawn. The declaration test
    // above is what fails if this frame samples the placeholder depth.
    Arcane::OverlayPrimitiveList open;
    open.lines.push_back(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 1.0f,
        Arcane::OverlayDepthClass::DepthTested,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)));
    const std::vector<unsigned char> bare = shoot(open, false);
    const Rgba blue = At(bare, kW, 10u, 10u);
    CHECK(blue.b == 255);
    CHECK(Arcane::RenderErrorCount() == before);
}

// Review Focus 3, GPU half. An opaque (alpha 1) display-referred pixel is
// identical with and without a post chain that tints the scene, because the
// overlay blends after the tonemap and alpha 1 replaces the destination. A
// background pixel the line does not cover changes. The CPU test above is the
// partial-alpha nonlinearity; this one is the pass order.
void CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    // The compile path is MaterialSourceTest.cpp:624-672, in post mode
    // (externalInput true, the sentinel case at :434-450). Bytes are copied
    // into PostChainDesc the way PostChainCache.cpp:271-282 publishes them.
    // CompileNow runs in the test, before either RenderFrame. PrepareChain's
    // DeviceWaitIdle (FullscreenNodes.cpp:867-871) runs only when m_ready is
    // already true; this chain is bound once, so that branch does not run.
    Arcane::ShaderSourceProvider provider;
    provider.AddRoot(Arcane::Test::RepoRoot() / "data" / "shaders");
    const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
    REQUIRE(templateText.has_value());

    const char* shade =
        "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv) * 0.25; }\n";
    const std::uint32_t sceneIn[] = { Arcane::kSceneInput };
    const Arcane::MaterialChainPassDesc authored[] = { { shade, sceneIn } };
    Arcane::MaterialChainBuildResult built = Arcane::BuildMaterialChainSource(
        *templateText, authored, "tint_quarter", {}, /*externalInput=*/true);
    REQUIRE(built.Ok());
    REQUIRE(built.hlsl.size() == 1u);
    REQUIRE(built.passInputs.size() == 1u);
    REQUIRE(built.passInputs[0].size() == 1u);
    REQUIRE(built.passInputs[0][0] == Arcane::kSceneInput);

    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(0.0));
    auto compileStage = [&](const char* entry, const char* profile) {
        Arcane::ShaderCompileRequest req;
        req.debugName = "tint_quarter.hlsl";
        req.sourceUtf8 = built.hlsl[0];
        req.entry = entry;
        req.profile = profile;
        const Arcane::ShaderCompileResult cr = compiler.CompileNow(req);
        REQUIRE(cr.AllSucceeded());
        const Arcane::ShaderTargetResult& target =
            backend == Arcane::GraphicsBackend::D3D12 ? cr.dxil : cr.spirv;
        REQUIRE(target.succeeded);
        REQUIRE_FALSE(target.bytecode.empty());
        return std::make_shared<const std::vector<std::uint8_t>>(target.bytecode);
    };
    auto templ = std::make_shared<Arcane::MaterialTemplate>(std::move(built.templ));
    Arcane::PostChainDesc chain;
    chain.templ = templ;
    chain.instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    chain.chainInputSlots = built.chainInputSlots;
    Arcane::PostChainPassDesc pass;
    pass.vsBytes = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
    pass.psBytes = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
    pass.inputs = built.passInputs[0];
    chain.passes.push_back(std::move(pass));
    compiler.Shutdown();

    PixelVehicle v = MakeVehicle(backend);
    Arcane::OverlayPrimitiveList list;
    list.lines.push_back(ScreenLine(4.0f, 10.0f, 16.0f, 10.0f, 0.0f,
        Arcane::OverlayDepthClass::AlwaysOnTop,
        Arcane::OverlayColorSpace::DisplayReferred,
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)));

    auto shoot = [&](const Arcane::PostChainDesc* post) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.overlay = &list;
        frame.post = post;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };
    const std::vector<unsigned char> noPost = shoot(nullptr);
    const std::vector<unsigned char> withPost = shoot(&chain);

    // (10, 10) is inside the 8px-thick segment. (4, 4) is outside it
    // (the quad covers y in [6, 14]) and is the tonemapped clear colour.
    const Rgba overlayA = At(noPost, kW, 10u, 10u);
    const Rgba overlayB = At(withPost, kW, 10u, 10u);
    CHECK(overlayA.r == 0);
    CHECK(overlayA.g == 255);
    CHECK(overlayA.b == 0);
    CHECK(overlayA.r == overlayB.r);
    CHECK(overlayA.g == overlayB.g);
    CHECK(overlayA.b == overlayB.b);

    const Rgba worldA = At(noPost, kW, 4u, 4u);
    const Rgba worldB = At(withPost, kW, 4u, 4u);
    CHECK((worldA.r != worldB.r || worldA.g != worldB.g || worldA.b != worldB.b));
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("overlay depth class and colour space (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckOverlayClasses(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("overlay depth class and colour space (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckOverlayClasses(Arcane::GraphicsBackend::Vulkan);
}
TEST_CASE("post chain does not grade an opaque overlay pixel (d3d12)", "[gpu][overlay][shadercompile][d3d12]")
{
    CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("post chain does not grade an opaque overlay pixel (vulkan)", "[gpu][overlay][shadercompile][vulkan]")
{
    CheckPostDoesNotGradeOverlay(Arcane::GraphicsBackend::Vulkan);
}

// Review Focus 5, perspective half. The screen-space lines above write clip.w = 1,
// so fragD = SV_Position.z / SV_Position.w still equals SV_Position.z and those
// checks stay green under the wrong formula. This one does not.
//
// FillCamera (eye z = 4, fovY 60, near 0.1, far 100, perspectiveRH_ZO) puts the
// cube's front face (world z = +1, view depth 3) at ndc.z ≈ 0.968. A world point
// on z = 0 has view depth 4 and ndc.z ≈ 0.976, farther than the front face and
// nearer than the depth clear of 1. The wrong formula divides that ndc.z by
// SV_Position.w (1/clip.w), producing ≈ ndc.z * viewDepth ≈ 3.9, which is past
// the clear and discards the unoccluded axis as well as the in-front line.
void CheckPerspectiveGridDepth(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    GpuSceneWorld w;
    w.AddMesh(cubeId, cube);
    w.Spawn(glm::vec3(0.0f), cubeId);
    w.Schedulers();

    Arcane::MeshSceneDesc scene;
    FillCamera(scene);
    Arcane::ViewTransform view;
    view.view       = scene.view;
    view.projection = scene.projection;
    view.viewport   = glm::uvec2{ kW, kH };

    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, view, vis);
    Arcane::GpuSceneMirror mirror;
    Arcane::GpuSceneFrame gpu;
    Arcane::GpuSceneSync(w.reg, mirror, /*deviceSyncedGeneration*/ 0u, gpu.stage);
    Arcane::BuildGpuSceneFrame(mirror, &vis, w.reg.GetResource<Arcane::MeshTable>(), view, gpu);
    for (Arcane::GpuInstance& row : gpu.stage.values)
        row.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->SetMeshSupply(SupplyOne(cubeId, cube));
    scene.scene = &gpu;

    auto shoot = [&](const Arcane::OverlayPrimitiveList& list) {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh = &scene;
        frame.overlay = &list;
        RenderOne(*v.ctx, frame);
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(width, height, rgba));
        REQUIRE(width == kW);
        REQUIRE(height == kH);
        return rgba;
    };

    // Axis only. Camera at y = 0 looking down -Z, so the world-X axis projects
    // onto y = kH/2. The cube's front face covers about x in [52, 108]; x = 10
    // is beside it. fwidth is about one pixel, so accept the midline and its
    // two neighbours. The wrong divide discards all three.
    Arcane::OverlayPrimitiveList grid;
    grid.hasGrid3d = true;
    grid.grid3d.view = view;
    // XY, not XZ: FillCamera's eye is at (0, 0, 4) with +Y up, so the XZ plane
    // (y = 0) is edge-on and rasterises nothing. The XY plane is z = 0, view depth 4.
    grid.grid3d.SetPlane(Arcane::GridSceneDesc::Plane::XY);
    grid.grid3d.minorSpacing = 1.0f;
    grid.grid3d.majorEvery = 8.0f;
    grid.grid3d.fadeDistance = 1000.0f;
    grid.grid3d.minHalfExtent = 20.0f;
    grid.grid3d.extentPerAltitude = 1.0f;
    grid.grid3d.minorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    grid.grid3d.majorColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    grid.grid3d.axisUColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    grid.grid3d.axisVColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<unsigned char> gridPx = shoot(grid);
    const Rgba centre = At(gridPx, kW, kW / 2u, kH / 2u);
    CHECK(centre.r > centre.g + 60);
    bool axis = false;
    for (std::uint32_t y = (kH / 2u) - 1u; y <= (kH / 2u) + 1u; ++y)
    {
        const Rgba px = At(gridPx, kW, 10u, y);
        if (px.g > 200 && px.g < 250 && px.r < 80)
            axis = true;
    }
    CHECK(axis);

    auto worldLine = [](float z) {
        Arcane::OverlayLine line{};
        line.a = { -0.4f, 0.0f, z };
        line.b = {  0.4f, 0.0f, z };
        line.thickness = 0.5f;
        line.depth = Arcane::OverlayDepthClass::DepthTested;
        line.space = Arcane::OverlayColorSpace::DisplayReferred;
        line.color = { 0.0f, 1.0f, 0.0f, 1.0f };
        line.screenSpace = false;
        return line;
    };
    Arcane::OverlayPrimitiveList inFront;
    inFront.lines.push_back(worldLine(2.0f));
    const Rgba front = At(shoot(inFront), kW, kW / 2u, kH / 2u);
    CHECK(front.r == 0);
    CHECK(front.g == 255);
    CHECK(front.b == 0);

    Arcane::OverlayPrimitiveList behind;
    behind.lines.push_back(worldLine(-2.0f));
    const Rgba back = At(shoot(behind), kW, kW / 2u, kH / 2u);
    CHECK(back.r > back.g + 60);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("a perspective grid axis is hidden on the cube and visible beside it (d3d12)", "[gpu][overlay][d3d12]")
{
    CheckPerspectiveGridDepth(Arcane::GraphicsBackend::D3D12);
}
TEST_CASE("a perspective grid axis is hidden on the cube and visible beside it (vulkan)", "[gpu][overlay][vulkan]")
{
    CheckPerspectiveGridDepth(Arcane::GraphicsBackend::Vulkan);
}
```

`compile-shaders.bat` is an explicit list. The test also reads the bat and requires both lines:

```cpp
CHECK(bat.find("call :compile overlay vs_main vs_6_5 overlay_vs") != std::string::npos);
CHECK(bat.find("call :compile overlay ps_main ps_6_5 overlay_ps") != std::string::npos);
CHECK(bat.find("call :compile overlay grid_vs_main vs_6_5 overlay_grid_vs") != std::string::npos);
CHECK(bat.find("call :compile overlay grid_ps_main ps_6_5 overlay_grid_ps") != std::string::npos);
```

- [ ] **Step 2: Build (ArcaneRender's prebuild compiles the shaders), run the overlay tag.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[overlay]"
```

Expected: FAIL (types missing, or the node is absent, or the GPU pixels do not match).

- [ ] **Step 3: Implement the overlay.**

Move `struct GridSceneDesc` (`GridNode.hpp:96-156`) verbatim into `GridSceneDesc.hpp` under `namespace Arcane`. Keep `SetPlane` and the three `ARC_CONSTANT` axis colours. `GridNode.hpp` includes that header.

`OverlayPrimitive.hpp` is the three enums and three structs in the Interfaces block. No NRI, no Astra.

`data/shaders/tonemap_operator.hlsl` contains the two functions cut from `tonemap.hlsl:37-50` (with the comment block `:43-46` between them), byte for byte, including the constants `2.51, 0.03, 2.43, 0.59, 0.14` and the `LinearToSrgb` body. `tonemap.hlsl` replaces those two function bodies with `#include "tonemap_operator.hlsl"`. `ps_main` stays:

```hlsl
float3 linearColor = g_Scene.Sample(g_Sampler, input.uv).rgb;
float3 display = LinearToSrgb(ACESFilmic(linearColor));
return float4(display, 1.0);
```

`data/shaders/overlay.hlsl`:

```hlsl
#include "tonemap_operator.hlsl"

struct VSIn
{
    float3 pos         : POSITION;
    float4 color       : COLOR0;
    float  screenSpace : TEXCOORD0;   // 1 = pixels, y down; 0 = world metres
    float  linearSpace : TEXCOORD1;   // 1 = run the operator; 0 = display-referred
    float  depthTest   : TEXCOORD2;   // 1 = sample-and-discard; 0 = always on top
};

struct VSOut
{
    float4 clip        : SV_Position;
    float4 color       : COLOR0;
    float  linearSpace : TEXCOORD0;
    float  depthTest   : TEXCOORD1;
};

cbuffer OverlayCB : register(b0)
{
    float4x4 g_ViewProjection;
    float2   g_Viewport;             // pixels
    float    g_DiscardIfGreater;     // 1 = ForwardZ (discard when frag > scene)
    float    g_HasDepth;             // 1 = sample g_Depth; 0 = draw unoccluded
};

Texture2D<float> g_Depth : register(t0);
SamplerState     g_Point : register(s0);

VSOut vs_main(VSIn input)
{
    VSOut o;
    if (input.screenSpace > 0.5)
    {
        float2 ndc;
        ndc.x = input.pos.x * (2.0 / g_Viewport.x) - 1.0;
        ndc.y = 1.0 - input.pos.y * (2.0 / g_Viewport.y);
        o.clip = float4(ndc, input.pos.z, 1.0);
    }
    else
    {
        o.clip = mul(g_ViewProjection, float4(input.pos, 1.0));
    }
    o.color = input.color;
    o.linearSpace = input.linearSpace;
    o.depthTest = input.depthTest;
    return o;
}

float4 ps_main(VSOut input) : SV_Target0
{
    if (g_HasDepth > 0.5 && input.depthTest > 0.5)
    {
        float2 uv = input.clip.xy / g_Viewport;
        float sceneD = g_Depth.Sample(g_Point, uv);
        // SV_Position.z is the post-divide depth. SV_Position.w is 1/clip.w.
        float fragD = input.clip.z;
        bool farther = (g_DiscardIfGreater > 0.5) ? (fragD > sceneD) : (fragD < sceneD);
        if (farther) discard;
    }
    float3 rgb = input.color.rgb;
    if (input.linearSpace > 0.5)
        rgb = LinearToSrgb(ACESFilmic(rgb));
    return float4(rgb, input.color.a);
}
```

The CPU sets `g_DiscardIfGreater` to `1.0f` when `NearerOrEqualOp(kSelectedDepthConvention) == nri::CompareOp::LESS_EQUAL`, else `0.0f`. `g_HasDepth` is `1` only when the frame's mesh pass produced `handles.depth`. Each vertex's `linearSpace` is `1` for `OverlayColorSpace::Linear` and `0` for `DisplayReferred`. Each vertex's `screenSpace` is `1` when `OverlayLine::screenSpace` (or the fill's) is true. Each vertex's `depthTest` is `1` for `OverlayDepthClass::DepthTested` and `0` for `AlwaysOnTop`.

`OverlayNode::Record` expands every line into two triangles before the draw. `thickness == 0` emits nothing. Otherwise `d = b.xy - a.xy`, `n = (-d.y, d.x) / length(d) * (thickness * 0.5)`, and the four corners are `a.xy ± n` and `b.xy ± n`, each with `z = a.z`. Screen-space `thickness` is pixels. World-space `thickness` is metres and the same corner math runs in world units with `screenSpace` 0, so the projected `clip.z` is what the discard compares. A fill `Rect` is the same two triangles over the box whose min corner is `a.xy` and whose size is `size`, at `z = a.z`. `Triangle` is one triangle `a, b, c`. `Circle` is a triangle fan of 16 segments of radius `radius` around `a.xy` at `z = a.z`. Gizmo lines and fills both go through this expansion; their `space` is `DisplayReferred` and their `depth` is `AlwaysOnTop`.

The analytic 3D grid is a second draw in the same pass: six vertices, no vertex buffer, the quad math from `grid.hlsl`'s `vs_main` / `ps_main` copied into `overlay.hlsl` as `grid_vs_main` / `grid_ps_main`. `compile-shaders.bat` gains two more lines beside the overlay pair:

```bat
call :compile overlay grid_vs_main vs_6_5 overlay_grid_vs || exit /b 1
call :compile overlay grid_ps_main ps_6_5 overlay_grid_ps || exit /b 1
```

`grid_vs_main` writes `depthTest = 1` and `clip = mul(g_ViewProjection, float4(world, 1.0))`, the same perspective quad `grid.hlsl`'s `vs_main` (`data/shaders/grid.hlsl:69`) builds. `grid_ps_main` computes the same linear grid colour `grid.hlsl`'s `ps_main` computes today, then `return float4(LinearToSrgb(ACESFilmic(linear.rgb)), linear.a);`. Its discard guard is the same six lines as `ps_main`, including `float fragD = input.clip.z;`. Paste that assignment into both functions so the source scan's second hit exists. Do not change `grid.hlsl` itself: `AddGridNode`'s direct test still loads `grid_vs` / `grid_ps` (`GridNode.cpp:37-38`) for a linear canvas, and that path is no longer on the frame.

Draw order inside `OverlayNode::Record`: 3D grid (when `hasGrid3d` or `shape.grid`), then lines and fills whose `depth` is `DepthTested`, then lines and fills whose `depth` is `AlwaysOnTop`. Depth write stays off (the pass has no depth attachment). Blend is straight alpha, source-over, on the tonemap target.

`OverlayNode::CreateBindings` is this node's own layout, matching `overlay.hlsl` (`b0`, `t0`, `s0`, all implicit space 0). Do not copy `GridNode::CreateBindings` (`GridNode.cpp:109-148`): that layout is one `CONSTANT_BUFFER` at `b1`, `registerSpace` 1, and `PoolSizes` (`:100-102`) says it has no textures and no samplers. `NriGraphContext` owns the node the way it owns `GridNode`: a `std::unique_ptr<OverlayNode> m_overlay` beside `m_grid` (`NriGraphContext.hpp:1479`), an `OverlayNode* Overlay()` accessor beside `Grid()` (`:1024`), construction beside `m_grid = GridNode::Create` (`NriGraphContext.cpp:470`), and `m_overlay->Release(graves, fence)` beside `m_grid->Release` (`:831-832`). `AddOverlayNode` reaches the node through `context->Overlay()`. Any new numeric `constexpr` in `OverlayNode.cpp` or `OverlayBatchCollector.hpp` (the 16 circle segments, the vertex stride) carries an `ARC_CONSTANT("...")` marker on the line above, or `ConstantGuardTest.cpp`'s `[guard]` scan fails the suite gate.

The layout follows `OutlineNode::CreateBindings` (`PickOutlineNodes.cpp:736-794`) plus a sampler, and the register-space rule at `FullscreenNodes.cpp:142-147` (`rootConstantNum`, `rootDescriptorNum`, and `rootSamplerNum` stay 0; `rootRegisterSpace` is 0 because the set's `registerSpace` is 0). Value-initialise every desc, then assign fields, the same dedup contract those two functions state.

- Range 0: `baseRegisterIndex` 0, `CONSTANT_BUFFER`, `descriptorNum` 1, `VERTEX_SHADER | FRAGMENT_SHADER`. The vertex shader reads `g_ViewProjection` and `g_Viewport`. The pixel shader reads `g_DiscardIfGreater` and `g_HasDepth`.
- Range 1: `baseRegisterIndex` 0, `TEXTURE`, `descriptorNum` 1, `FRAGMENT_SHADER`. This is `g_Depth` at `t0`.
- Range 2: `baseRegisterIndex` 0, `SAMPLER`, `descriptorNum` 1, `FRAGMENT_SHADER`. This is `g_Point` at `s0`.

Range order is CBV, then SRV, then sampler. `OutlineNode`'s comment (`PickOutlineNodes.cpp:748-754`) is why: D3D12 merges consecutive ranges of the same type, and these three types differ, so this is three ranges. On Vulkan, NRI shifts the HLSL register numbers by the device's `vkBindingOffsets` (`b=256`, `t=0`, `s=128`). Write the HLSL numbers.

`descriptorSetMaxNum`, `constantBufferMaxNum`, `textureMaxNum`, and `samplerMaxNum` are each `FramesInFlight()`. One set per frame slot: the constant bytes and the depth texture identity change per frame. Allocate with `AllocateDescriptorSets` the way `TonemapNode` does (`FullscreenNodes.cpp:1217`).

The sampler is `TonemapNode`'s point sampler (`FullscreenNodes.cpp:1154-1162`): `Filter::NEAREST` for min, mag, and mip, `AddressMode::CLAMP_TO_EDGE` on all three axes, `mipMax` 16. Create it once and write it into every set at `rangeIndex` 2.

The constant-buffer view is created once per slot and written once at `rangeIndex` 0, the pattern at `PickOutlineNodes.cpp:856-884`. Record copies that slot's bytes into the arena. The view's `(buffer, offset)` does not change.

The graph does not create a shader-resource view of scene depth. `RealizeAttachmentViews` (`RenderGraphExec.cpp:873-876`) creates only `DEPTH_STENCIL_ATTACHMENT` or `COLOR_ATTACHMENT`. `Read(..., RgUsage::ShaderRead)` adds `TextureUsageBits::SHADER_RESOURCE` (`RenderGraphExec.cpp:218`) and creates no view. At record, `context.Resolve` (`RenderGraph.hpp:235`) yields the `nri::Texture*`. `OverlayNode::EnsureView` is `OutlineNode::EnsureView` (`PickOutlineNodes.cpp:1019-1052`): `TextureView::TEXTURE`, `format = core.GetTextureDesc(*texture).format`, `mipNum` 1, `layerNum` 1. Bind that view at `rangeIndex` 1 when the texture pointer changes, the same update as `PickOutlineNodes.cpp:1088-1101`.

Scene depth defaults to `D32_SFLOAT` (`NriGraphContext.hpp:302`; `nri::Format::D32_SFLOAT` is `NRIDescs.h:339`, `R32_SFLOAT` is `:248`). NRI's D3D12 backend patches `DXGI_FORMAT_D32_FLOAT` to `DXGI_FORMAT_R32_FLOAT` for a `TEXTURE` view (`DescriptorD3D12.hpp:9-10`, and the `TEXTURE_2D` case at `:148-163`). `Texture2D<float>` matches that `R32_SFLOAT` view. Do not create a `DEPTH_STENCIL_ATTACHMENT` view and bind it at `t0`.

`overlay.hlsl` always declares `t0`. An unbound `t0` is a validation failure, including the missing-depth witness and every `RenderErrorCount()` check. The node therefore owns a 1x1 `R32_SFLOAT` texture created in `Create`, the committed-texture shape at `FullscreenNodes.cpp:212-220` with `format = nri::Format::R32_SFLOAT` and `usage = SHADER_RESOURCE`, plus one `TextureView::TEXTURE` of it. Upload a single `0.0f` through `HelperInterface` the way `CreateFallbackTexels` uploads its texels (`FullscreenNodes.cpp:235-260`), `rowPitch` 4 and `slicePitch` 4, so the resource is initialised even though the shader does not sample it. When the depth handle is invalid, do not `Read` a graph depth, set `g_HasDepth` to 0, and bind that dummy at `t0`. When the handle is valid, `Read(depth, RgUsage::ShaderRead)`, `EnsureView` the resolved scene depth, and set `g_HasDepth` to 1. The pipeline still has no depth attachment (`depthFormat` `UNKNOWN`, depth write off).

`BuildFrame` copies `shape.overlay = frame.overlay` and still copies `shape.grid`. `DeclareGraphFrame` deletes the block at `:1398-1403` (`if (shape.grid) AddGridNode(...)`). After `AddTonemapNode` returns `handles.backbuffer` and before gameui (`:1480`):

```cpp
const bool wantOverlay = (shape.grid != nullptr) ||
    (shape.overlay != nullptr &&
     (!shape.overlay->lines.empty() || !shape.overlay->fills.empty() || shape.overlay->hasGrid3d));
if (wantOverlay)
{
    AddOverlayNode(graph, context, handles.backbuffer,
                   wantsMesh ? handles.depth : RgTexture{},
                   shape.grid, shape.overlay, shape.canvasWidth, shape.canvasHeight);
}
```

`WasRead`, next to `WasWritten` (`RenderGraph.cpp:892`):

```cpp
bool RenderGraph::WasRead(RgTexture texture) const
{
    const std::size_t slot = DecodeAndValidateSlot(texture.index, m_textures.size());
    if (slot == kNoSlot)
        return false;
    for (const Access& access : m_accesses)
        if (access.isTexture && access.resourceIndex == slot && !access.isWrite)
            return true;
    return false;
}
```

Declare it beside `WasWritten` (`RenderGraph.hpp:699`).

`AddOverlayNode` declares node `"overlay"`, `NodeKind::Raster`, colour attachment `handles.backbuffer` with load-op `Load` (the tonemap already wrote it), no depth attachment. When `depth` is a valid handle it calls `builder.Read(depth, RgUsage::ShaderRead)`. When `depth` is invalid it does not `Read` any depth texture and the recorded constants set `g_HasDepth` to 0. It does not read the placeholder depth node `shape.depth` creates.

`OverlayNode.hpp` includes `<Arcane/Modules/ArcaneRenderApi.hpp>` first and exports the node the way `GridNode.hpp` does (`class ARC_RENDER_API GridNode`, `:158`; `ARC_RENDER_API void AddGridNode(...)`, `:283`): `class ARC_RENDER_API OverlayNode` and `ARC_RENDER_API void AddOverlayNode(...)`. `GridSceneDesc.hpp` and `OverlayPrimitive.hpp` are header-only and carry no macro. All three new headers sit in ArcaneRender; the module's `src/**` glob picks them up.

Rewrite all three sections of the grid frame-shape case (`RenderGraphTest.cpp:7077-7178`); there is no node named `"grid"` after this task.

- With a mesh and a grid (`:7089-7137`): names `gbuffer, lighting, batch2d, gpuscene-sync, mesh-cull, mesh, taa, tonemap, overlay` (count and `nodes.size()` 9). `NameOf(handles.depth) == "depth"`, `WasWritten(depth)`, `WasRead(depth)`, and `DepthLoadOp` of the overlay node is null. `transients.size()` stays 2 (canvas + depth; the backbuffer is imported) and `poolSlotCount` stays 2. The depth lifetime is `first == 5` (mesh) and `last == 8` (overlay). Delete the "grid carries no barrier" check (`:7130-7136`): the overlay `Read`s depth as `ShaderRead`, so `compiled.nodes[8].preBarriers` holds exactly one barrier, the depth texture moving to the shader-read state; the backbuffer is in the `ColorWrite` state the tonemap left, so it contributes none.
- With a grid and no mesh (`:7139-7161`): names `gbuffer, lighting, batch2d, taa, tonemap, overlay` (count and size 6); `IsHandleValid(handles.depth)` false and nothing reads a depth; one transient, one pool slot; `compiled.nodes[5].preBarriers.empty()` (the backbuffer is already `ColorWrite`).
- With a mesh and no grid (`:7163-7176`): count 8, `NodeName(3)` gpuscene-sync, `(4)` mesh-cull, `(5)` mesh, `(7)` tonemap, and no `overlay` node.

**Editor submissions move in this same commit.** Otherwise the grid disappears until Task 9.

`OverlayBatchCollector.hpp` (ArcaneRender, header-only; includes `<Arcane/Render/Batcher2D.hpp>` and `<Arcane/Render/OverlayPrimitive.hpp>`) is the bridge: a `Batcher2D` whose `Line`, `Circle`, and `Rect` append overlay primitives instead of quads, so the producers that already draw through a `Batcher2D` keep their code and emit overlay primitives in the pixel space they already work in. The class sits in `namespace Arcane`. Every pure virtual is defined. New virtuals stay at the end of `Batcher2D`; this subclass does not change that class. Task 9 reuses it inside extract.

```cpp
class OverlayBatchCollector final : public Batcher2D
{
public:
    explicit OverlayBatchCollector(OverlayPrimitiveList& out,
                                   OverlayDepthClass depth = OverlayDepthClass::AlwaysOnTop)
        : m_out(out), m_depth(depth) {}

    void Begin(std::uint32_t, std::uint32_t) override {}
    void SetLayer(std::uint16_t, std::uint16_t) override {}
    void Quad(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4, float) override {}
    void Glyph(glm::vec2, glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
    void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
    void End() override {}
    Batch2DStats Stats() const override { return {}; }
    void QuadWorld(std::uint16_t, const Guid&, const std::array<glm::vec3, 4>&,
                   glm::vec2, glm::vec2, glm::vec4) override {}
    void CircleWorld(glm::vec3, glm::vec3, glm::vec3, float, glm::vec4) override {}
    void SetViewProjection(const glm::mat4&) override {}

    void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 color) override
    {
        OverlayLine line{};
        line.a = glm::vec3(a, 0.0f);
        line.b = glm::vec3(b, 0.0f);
        line.thickness = thickness;
        line.color = color;
        line.depth = m_depth;
        line.space = OverlayColorSpace::Linear;
        line.screenSpace = true;
        m_out.lines.push_back(line);
    }

    void Circle(glm::vec2 center, float radius, glm::vec4 color) override
    {
        OverlayFill fill{};
        fill.kind = OverlayPrimKind::Circle;
        fill.a = glm::vec3(center, 0.0f);
        fill.radius = radius;
        fill.color = color;
        fill.depth = m_depth;
        fill.space = OverlayColorSpace::Linear;
        fill.screenSpace = true;
        m_out.fills.push_back(fill);
    }

    void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color, float) override
    {
        OverlayFill fill{};
        fill.kind = OverlayPrimKind::Rect;
        fill.a = glm::vec3(pos, 0.0f);
        fill.size = size;
        fill.color = color;
        fill.depth = m_depth;
        fill.space = OverlayColorSpace::Linear;
        fill.screenSpace = true;
        m_out.fills.push_back(fill);
    }

private:
    OverlayPrimitiveList& m_out;
    OverlayDepthClass     m_depth;
};
```

- `SubmitSceneToBatcher` (`:1866`) clears `m_overlayPrims` (an `Arcane::OverlayPrimitiveList` member of `EditorApp`, in `EditorApp.hpp`, alive until the next `SubmitSceneToBatcher`) and stops drawing the grid, the physics overlay, and the camera rect into the scene batcher `b`:
  - The 2D grid: `DrawGrid2D` (`:1896`) is called with `OverlayBatchCollector grid(m_overlayPrims, Arcane::OverlayDepthClass::DepthTested)` instead of `b`. `DrawGrid2D` already projects each world segment to pixels (`ViewportGrid.cpp:166-174`), so the lines land `screenSpace = true` with `z = 0`, `Linear`, `DepthTested`. With `z = 0` they sit on the near plane in this interim, so no mesh occludes them; Task 9 replaces them with world-space lines from `EmitGrid2D`.
  - Physics debug (`:1907-1936`; it reads `PhysicsWorld2D` and draws through `Detail::Physics2D::Access::Solver` at `:1936`) passes `OverlayBatchCollector debug(m_overlayPrims)` to `DrawPhysicsDebug` instead of `b`. Its primitives are already pixels through the view's `Affine2D` (`:1920-1926`): `AlwaysOnTop`, `Linear`, `screenSpace = true`. Physics debug has no `SetLayer` today; the tag is what keeps it above the grid.
  - The camera rect (`:1963-1991`) is outside the physics block's scope (`:1906-1938`), so it makes its own `OverlayBatchCollector rect(m_overlayPrims)` (default `AlwaysOnTop`) and calls `rect.Line` instead of `b.Line` (`:1991`); its corners are already `WorldToScreen` pixels: `AlwaysOnTop`, `Linear`, `screenSpace = true`.
  - Last, it appends `m_gizmoPrims`' lines and fills to `m_overlayPrims`.
- The gizmo moves out of the panel phase so it is not a frame late. Today it is painted by the `gizmoOverlay` lambda (`:3723`) inside `DrawViewportPanelPhase` (`:501`), which runs after `RenderSceneToViewport` (`:480`). Add `EditorApp::BuildGizmoOverlayPrims()` (declared in `EditorApp.hpp`), called from `MainLoop` immediately before `RenderSceneToViewport()` (`:480`). It runs the lambda's body (the same gate, handle mask, size, and view as the hit-test, and the draw-time `GetComponent` now at `:3730`) through a gizmo sink that fills `m_gizmoPrims` (an `OverlayPrimitiveList` member) from the same `Line`/`Triangle`/`Rect`/`Circle` callbacks, tagged `AlwaysOnTop`, `DisplayReferred`, `screenSpace = true`, positions in viewport pixels (image origin `(0, 0)`, not the ImGui panel origin). The lambda no longer draws; pass none, or an empty one, to `DrawViewportPanel`. `BuildGizmoOverlayPrims` is not a host-gate function. The interaction `GetComponent` calls at `:1250`, `:1292`, `:1348` stay. Removing the ImGui sink draw here is what prevents a double draw once Task 9 also emits those primitives.
- `ArmGraphViewportFrame` (`:2052`) sets `vp.overlay = &m_overlayPrims`.
- Interim gap, recorded: the scene-save cover (`CaptureGraphViewportPng`, `EditorApp.cpp:2056-2090`) calls `SubmitSceneToBatcher` but not `ArmGraphViewportFrame`, so from this task until Task 9 the cover has no 2D grid, physics overlay, or camera rect. The cover is not a golden; Task 9 restores those three through the packet.

`FrameDesc::grid` stays the 3D grid desc the editor already fills, and this task's node reads that pointer on a frame that did not call `BindPacket`. Task 9 stops the editor writing `vp.grid` (`EditorAppFrame.cpp:2118-2129`), deletes `m_overlayPrims` and the collector calls in `SubmitSceneToBatcher`, and stops pointing `FrameDesc::overlay` at that list; `m_gizmoPrims` and `BuildGizmoOverlayPrims` stay. Runtime frames leave `overlay` null and `grid` null, so they do not declare the node and `runtime-scene` node order is Task 2's order. This task's GPU cases set `frame.overlay` and do not call `BindPacket`; that path has to keep drawing.

Do not run the golden gate. `editor-ui` and `editor-ui-perspective` will differ on grid pixels. That is expected until Task 13.

- [ ] **Step 4: Run the overlay tests and the device-less graph tests.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[overlay]"
.\ArcaneTests.exe "[nri]"
```

Expected: PASS. `[gpu]` cases require a GPU. Record the seed. `RenderErrorCount` stays 0.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Client/ArcaneRender/src/Arcane/Render/GridSceneDesc.hpp Source/Client/ArcaneRender/src/Arcane/Render/OverlayPrimitive.hpp Source/Client/ArcaneRender/src/Arcane/Render/OverlayBatchCollector.hpp Source/Programs/ArcaneEditor/src/App/EditorApp.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/GridNode.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/OverlayWitnessTest.cpp Tests/ArcaneTests/src/RenderGraphTest.cpp data/shaders/tonemap_operator.hlsl data/shaders/tonemap.hlsl data/shaders/overlay.hlsl data/shaders/compile-shaders.bat
git commit -m "feat(render): post-tonemap overlay pass for grid, debug, and gizmo" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 4: RenderFramePacket and the in-flight ring

**Files:**
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCameraView.hpp`
- Modify: `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCamera.hpp:31-42` (delete the struct; include the new header beside `ViewTransform.hpp`)
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/VisibleSet.hpp`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/VisibilitySystem.hpp:48-84` (the two structs move; `BuildVisibleSet` at `:119` stays)
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.cpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/PostChainDesc.hpp`; Modify: `Source/Client/ArcaneRender/src/Arcane/Render/PostChainCache.hpp:56-79`
- Create: `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImDrawSnapshot.hpp` and `ImDrawSnapshot.cpp` (ruling PR-1)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderOverlayNode.hpp:29-33` (`RenderOverlayHooks` gains `snapshot`) (ruling PR-1)
- Modify: `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiHostHooks.cpp:17-22` (installs `snapshot`) (ruling PR-1)
- Test: `Tests/ArcaneTests/src/RenderFramePacketTest.cpp`
- Modify: `Tests/ArcaneTests/src/NriGraphPixelTest.cpp` (append the rebuild reach case after `CheckImGuiDrawsPastOnePool`)

**Interfaces:**
- Consumes: `VisibleEntry` / `VisibleSet` fields (`VisibilitySystem.hpp:48-84`), `GpuSceneFrame` (`GpuSceneTypes.hpp:276-290`), `Batch2DVertex` (`Batcher2D.hpp:127-133`), `Batch2DDrawSpan` (`:140-158`), `Batch2DDrained` (`:164-189`, non-owning, valid until the next `Begin()`), `GlobalParams` (`Source/Runtime/ArcaneAssets/src/Arcane/Material/GlobalParams.hpp:33-39`), `PickDrawable` (`PickEmit.hpp:47-71`, `Astra::Entity` by value), `OverlayPrimitiveList` from Task 3, `PostChainDesc` (`PostChainCache.hpp:67-79`), `ViewTransform` (`ViewTransform.hpp:50`), `SceneCameraView` (`SceneCamera.hpp:31` today; this task moves it), `ActiveSceneCamera`'s return type `std::optional<SceneCameraView>` (`SceneCamera.hpp:57`), `ActivePerspectiveSceneCamera`'s return type `std::optional<ViewTransform>` (`SceneCamera.hpp:206`), `ImTextureID` as `ImU64` (`imgui.h:345`), `ImTextureRef::_TexData` (`imgui.h:383`, public), `ImDrawCmd::GetTexID` (`imgui.h:4073-4081`, asserts when `_TexData != nullptr` and that object's `TexID` is still `ImTextureID_Invalid`), `ImDrawCallback` (`imgui.h:3303`), `ImDrawData` fields (`imgui.h:3590-3601`: `Valid`, `CmdListsCount`, `TotalIdxCount`, `TotalVtxCount`, `CmdLists`, `DisplayPos`, `DisplaySize`, `FramebufferScale`, `OwnerViewport`, `Textures`), `FramesInFlight()`, `kMaxFramesInFlight`. `ViewTransform` has no `operator==`; tests compare `.view`, `.projection`, and `.viewport`.
- Produces:
  - `struct SceneCameraView` in `SceneCameraView.hpp`, with the three fields that are at `SceneCamera.hpp:31-42` today: `glm::vec2 worldCenter`, `float halfHeight`, `ViewTransform view`.
  - `struct RenderOrigin { glm::vec3 translation{0.0f}; };`
  - Placement (ruling PR-1). ArcaneRender may not include `<imgui.h>`: ImGui is an optional vendor module behind ArcaneRender's `RenderOverlayHooks` seam (`Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderOverlayNode.hpp:17-36`; modularization spec s1.2, s1.5, R12), and ArcaneImGui depends on ArcaneRender, not the reverse. So the snapshot *types* are Render-owned plain data in `RenderFramePacket.hpp` (no imgui type in them), and the code that reads or writes imgui types -- `CopyImDrawSnapshot` and `ImDrawSnapshotRebuild` -- lives in ArcaneImGui's `Arcane/ImGui/ImDrawSnapshot.hpp/.cpp`, exported `ARC_IMGUI_API`. ExtractFrame (ArcaneRender) reaches the copy through a third `RenderOverlayHooks` member, `bool (*snapshot)(const ImDrawData* src, ImDrawSnapshot& out)`, which `InstallImGuiHostHooks` sets (ArcaneTests installs it in `Tests/ArcaneTests/src/test_main.cpp:25`; both hosts call it). A host without ImGui leaves the hook null and its packets carry empty snapshots. The behaviour below is unchanged.
  - `struct ImDrawListSnapshot` and `struct ImDrawSnapshot` as written in Step 3. One snapshot owns its lists. A command with `TexRef._TexData == nullptr` stores `ImTextureID` as `ImU64`, initialised to `0` (`ImTextureID_Invalid`). A command with `TexRef._TexData != nullptr` stores that pointer and leaves the `ImU64` at `0`. `hasTextures` distinguishes a null `ImDrawData::Textures` from an empty list. The pointer vector is a copy of the pointers, not a deep copy of `ImTextureData`.
  - `ARC_IMGUI_API bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out);` (ArcaneImGui) — `false` and an empty `out` when any command has `UserCallback != nullptr`. It does not call `GetTexID` on a managed command.
  - `class ARC_IMGUI_API ImDrawSnapshotRebuild` (ArcaneImGui) — builds an `ImDrawData` by direct field assignment. `CmdLists` point at `IM_NEW`'d `ImDrawList`s owned by the rebuild object. It never calls `AddDrawList`. `m_textures` is a member so `Data()->Textures` stays valid for the caller's lifetime.
  - `struct RenderFramePacket` with the fields listed in Step 3. `spriteView` is `std::optional<SceneCameraView>`. `meshView` is `std::optional<ViewTransform>`. No `Astra::Registry` member. The header includes `SceneCameraView.hpp`. It does not include `Astra/Registry/Registry.hpp`, `SceneCamera.hpp`, `VisibilitySystem.hpp`, `GridNode.hpp`, or `imgui.h`.
  - `class ARC_RENDER_API PacketRing` with `RenderFramePacket& BeginExtract();`, `RenderFramePacket& Latest();`, and `const RenderFramePacket& Slot(std::uint32_t extractsAgo) const;`.
  - `BeginExtract` returns the slot at `extractCount % FramesInFlight()`, clears that slot, then increments `extractCount`. `Latest()` returns that same slot.

`ImDrawData` has no clone. `AddDrawList` (`imgui.h:3606`) is the only insert helper, and it is not usable on a hand-filled list: `AddDrawListToDrawDataEx` (`imgui_draw.cpp:2303-2308`) asserts `_VtxWritePtr == VtxBuffer.Data + VtxBuffer.Size` and `_IdxWritePtr == IdxBuffer.Data + IdxBuffer.Size`, and (`:2326`) `_VtxCurrentIdx < 65536`. Those write pointers are left unset by a `VtxBuffer.push_back` the caller did itself. `ImDrawList::CloneOutput` (`imgui.h:3530`, `imgui_draw.cpp:503`) copies the buffers and still leaves the write pointers unset and `SharedData` null. The rebuild therefore `push_back`s each owned list onto `m_data.CmdLists` and increments `CmdListsCount`, `TotalVtxCount`, and `TotalIdxCount` itself (`imgui_draw.cpp:2335-2338` is what `AddDrawList` would have done). `ImGuiNri::RenderDrawData` (`ImGuiNri.cpp:782`) does not read `draw_data->Textures`. It reads `DisplaySize` (`:785`), `TotalVtxCount` (`:794`), each list's `VtxBuffer` / `IdxBuffer` (`:849-857`), `DisplayPos` (`:864`, `:935`), `FramebufferScale` (`:936`), `UserCallback` (asserted null at `:954`), `ClipRect`, `TexRef._TexData` (`:980-991`), `GetTexID` (`:993`), `ElemCount`, `IdxOffset`, and `VtxOffset` (`baseVertex = globalVtxOffset + cmd->VtxOffset`, `:1025`). A flat command vector would make that `VtxOffset` address the wrong buffer, so the snapshot keeps one vertex/index/command vector per list.

`NewFrameTexUpdates` (`ImGuiNri.cpp:772-779`) is what reads `drawData->Textures`, and it returns immediately when that pointer is null. `ImGuiNriNode::PrepareFrame` (`ImGuiNriNode.cpp:41-44`) calls it at declaration (`ImGuiNriNode.cpp:76-91`), before `Record` (`:58-60`). `ImGuiNri.hpp:185-205` says the upload submits and waits and must stay at declaration time. This plan does not service textures on the main thread before the snapshot is published: `NewFrameTexUpdates` blocks, and extract has no graveyard. The snapshot carries the pointer list. Service stays at declaration time, on the rebuilt `ImDrawData`. `ImGui::Render` assigns `Textures = &GetPlatformIO().Textures` (`imgui.cpp:6104`) before the graph is declared, so on the first frame and on an atlas rebuild `TexID` is still `ImTextureID_Invalid` when extract copies. Storing only `GetTexID()` at copy time would keep that invalid id, `GetTexID` would assert at record (`imgui.h:4080-4081`), and the unregistered guard (`ImGuiNri.cpp:980-991`) would drop the draw. Each copied command therefore keeps `TexRef._TexData` when the live command has one, and the raw `ImTextureID` only when `_TexData` is null (the user `AddImage` path; that path still stores `ImU64`). Rebuild writes `cmd.TexRef._TexData = managed` and leaves `_TexID` invalid when `managed` is non-null (`ImTextureRef::GetTexID`, `imgui.h:4068`, asserts they are not both set). Two stack rebuilds, `PrepareFrame` and `Record`, both point at the same `ImTextureData` objects, so the `SetTexID` from declaration is visible at record. The GPU still never reads packet memory.

`VisibleSet.hpp` includes `<Arcane/Math/Aabb.hpp>` (ArcaneCore), `<Arcane/Scene/Frustum.hpp>`, `<Arcane/Scene/ViewTransform.hpp>`, `<Astra/Entity/Entity.hpp>`, and `<vector>` (the paths `VisibilitySystem.hpp:24-28` uses). It does not include `Registry.hpp`. `VisibilitySystem.hpp` includes `VisibleSet.hpp`, deletes the two struct definitions, and keeps `SceneVisibility`, `MainVisibleSet`, and `BuildVisibleSet`.

- [ ] **Step 1: Write the failing test.**

`Tests/ArcaneTests/src/RenderFramePacketTest.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/ImGui/ImDrawSnapshot.hpp>   // CopyImDrawSnapshot / ImDrawSnapshotRebuild (ArcaneImGui) ruling PR-1

#include "Helpers/ConstantScan.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cstring>
#include <fstream>
#include <sstream>

TEST_CASE("a packet slot stays intact until FramesInFlight extracts have passed", "[nri][packet]")
{
    Arcane::LatchFramesInFlight();
    const std::uint32_t n = Arcane::FramesInFlight();
    REQUIRE(n >= 2u);
    REQUIRE(n <= Arcane::kMaxFramesInFlight);

    Arcane::PacketRing ring;
    Arcane::RenderFramePacket& first = ring.BeginExtract();
    CHECK(&ring.Latest() == &first);
    first.batchVertices.push_back(Arcane::Batch2DVertex{});
    first.batchVertices.back().pos = { 1.0f, 2.0f, 3.0f };
    first.origin.translation = { 0.0f, 0.0f, 0.0f };
    const Arcane::Batch2DVertex* span = first.batchVertices.data();
    const float kept = span->pos.x;

    for (std::uint32_t i = 0; i < n - 1; ++i)
    {
        Arcane::RenderFramePacket& slot = ring.BeginExtract();
        slot.batchVertices.push_back(Arcane::Batch2DVertex{});
        slot.batchVertices.back().pos = { 9.0f, 9.0f, 9.0f };
    }

    const Arcane::RenderFramePacket& still = ring.Slot(n - 1);
    REQUIRE(still.batchVertices.size() == 1u);
    CHECK(still.batchVertices[0].pos.x == kept);
    CHECK(still.origin.translation == glm::vec3(0.0f));
    // The span handed out at extract still addresses that slot's vector.
    CHECK(span == still.batchVertices.data());

    Arcane::RenderFramePacket& overwritten = ring.BeginExtract();
    CHECK(overwritten.batchVertices.empty());   // BeginExtract clears the recycled slot
}

TEST_CASE("mutating a source vector after the copy does not change the packet", "[nri][packet]")
{
    std::vector<Arcane::Batch2DVertex> source(1);
    source[0].pos = { 4.0f, 5.0f, 6.0f };
    Arcane::RenderFramePacket packet;
    packet.batchVertices = source;
    source[0].pos = { 0.0f, 0.0f, 0.0f };
    source.clear();
    CHECK(packet.batchVertices.size() == 1u);
    CHECK(packet.batchVertices[0].pos.x == 4.0f);
}

TEST_CASE("RenderFramePacket.hpp does not include the Astra registry", "[nri][packet]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.hpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("Registry.hpp") == std::string::npos);
    CHECK(text.find("Astra::Registry") == std::string::npos);
    CHECK(text.find("VisibilitySystem.hpp") == std::string::npos);
    CHECK(text.find("SceneCamera.hpp") == std::string::npos);
    CHECK(text.find("imgui.h") == std::string::npos);   // ruling PR-1
    CHECK(text.find("SceneCameraView.hpp") != std::string::npos);
    CHECK(text.find("optional<SceneCameraView> spriteView") != std::string::npos);
    CHECK(text.find("optional<ViewTransform> meshView") != std::string::npos);
}

TEST_CASE("spriteView is a SceneCameraView and meshView is a ViewTransform", "[nri][packet]")
{
    Arcane::RenderFramePacket packet;
    Arcane::SceneCameraView sprite;
    sprite.worldCenter = { 1.0f, 2.0f };
    sprite.halfHeight = 3.0f;
    sprite.view.view = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 0.0f, 0.0f));
    sprite.view.projection = glm::mat4(1.0f);
    sprite.view.viewport = { 8u, 9u };
    packet.spriteView = sprite;
    Arcane::ViewTransform mesh;
    mesh.projection = glm::mat4(2.0f);
    packet.meshView = mesh;
    REQUIRE(packet.spriteView.has_value());
    CHECK(packet.spriteView->worldCenter == glm::vec2(1.0f, 2.0f));
    CHECK(packet.spriteView->halfHeight == 3.0f);
    CHECK(packet.spriteView->view.view[3][0] == 4.0f);
    CHECK(packet.spriteView->view.viewport == glm::uvec2(8u, 9u));
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.meshView->projection[0][0] == 2.0f);
}

static void DummyImGuiCallback(const ImDrawList*, const ImDrawCmd*) {}

TEST_CASE("an ImDrawSnapshot matches the live draw data field for field and refuses a callback", "[nri][packet]")
{
    // A default ImDrawData has Textures == nullptr (imgui_draw.cpp:2284-2291).
    // That arm is not what ImGui::Render produces.
    ImDrawData cleared;
    Arcane::ImDrawSnapshot clearedSnap;
    REQUIRE(Arcane::CopyImDrawSnapshot(&cleared, clearedSnap));
    CHECK_FALSE(clearedSnap.hasTextures);
    CHECK(clearedSnap.textures.empty());
    Arcane::ImDrawSnapshotRebuild clearedRebuild(clearedSnap);
    CHECK(clearedRebuild.Data()->Textures == nullptr);
    CHECK(clearedRebuild.Data()->Valid);
    CHECK(clearedRebuild.Data()->CmdLists.Size == 0);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(128.0f, 128.0f);
    // Before NewFrame. NewFrame copies this into ImDrawListFlags_AllowVtxOffset
    // (imgui.cpp:5706-5707). A window list created afterwards carries the flag.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    // Matches the live backend (ImGuiNri.cpp:119). Without it NewFrame demands a
    // built font atlas (imgui_draw.cpp:2778).
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    // A window created without both sizes set by the API is hidden for its first
    // frame (imgui.cpp:8093-8095) and a hidden window is left out of the draw
    // data (imgui.cpp:6394-6395). Every window here is rendered on its first
    // frame, so each sets its position and size first.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("one");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(42), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(64.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("two");
    ImDrawList* list = ImGui::GetWindowDrawList();
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(7), ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f));
    // Chrome already consumed some of the 16-bit index space. Pad from the
    // current _VtxCurrentIdx, not from zero, up to but not over the boundary.
    // The following AddRectFilled's PrimReserve then crosses it
    // (imgui_draw.cpp:737-743) and the new command's VtxOffset is the buffer
    // size at the crossing (imgui_draw.cpp:646-659, AddDrawCmd at :518).
    const int pad = (1 << 16) - static_cast<int>(list->_VtxCurrentIdx) - 1;
    REQUIRE(pad > 0);
    list->PrimReserve(0, pad);
    for (int i = 0; i < pad; ++i)
    {
        list->_VtxWritePtr->pos = ImVec2(0.0f, 0.0f);
        list->_VtxWritePtr->uv = ImVec2(0.0f, 0.0f);
        list->_VtxWritePtr->col = IM_COL32_WHITE;
        ++list->_VtxWritePtr;
        ++list->_VtxCurrentIdx;
    }
    list->AddRectFilled(ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f), IM_COL32(255, 0, 0, 255));
    ImGui::End();
    ImGui::Render();

    ImDrawData* src = ImGui::GetDrawData();
    REQUIRE(src != nullptr);
    REQUIRE(src->Valid);
    REQUIRE(src->CmdLists.Size >= 2);
    REQUIRE(src->Textures != nullptr);
    bool sawOffset = false;
    for (const ImDrawList* live : src->CmdLists)
    {
        REQUIRE(live != nullptr);
        for (const ImDrawCmd& cmd : live->CmdBuffer)
            if (cmd.UserCallback == nullptr && cmd.VtxOffset > 0u)
                sawOffset = true;
    }
    REQUIRE(sawOffset);

    Arcane::ImDrawSnapshot snap;
    REQUIRE(Arcane::CopyImDrawSnapshot(src, snap));
    REQUIRE(snap.lists.size() == static_cast<std::size_t>(src->CmdLists.Size));
    CHECK(snap.hasTextures);
    REQUIRE(snap.textures.size() == static_cast<std::size_t>(src->Textures->Size));
    for (int i = 0; i < src->Textures->Size; ++i)
        CHECK(snap.textures[static_cast<std::size_t>(i)] == (*src->Textures)[i]);
    CHECK(snap.displayPos == glm::vec2(src->DisplayPos.x, src->DisplayPos.y));
    CHECK(snap.displaySize == glm::vec2(src->DisplaySize.x, src->DisplaySize.y));
    CHECK(snap.framebufferScale == glm::vec2(src->FramebufferScale.x, src->FramebufferScale.y));

    bool saw42 = false;
    bool saw7 = false;
    for (std::size_t li = 0; li < snap.lists.size(); ++li)
    {
        const ImDrawList* live = src->CmdLists[static_cast<int>(li)];
        const Arcane::ImDrawListSnapshot& copied = snap.lists[li];
        REQUIRE(copied.vertices.size() == static_cast<std::size_t>(live->VtxBuffer.Size));
        REQUIRE(copied.indices.size() == static_cast<std::size_t>(live->IdxBuffer.Size));
        REQUIRE(copied.cmds.size() == static_cast<std::size_t>(live->CmdBuffer.Size));
        if (live->VtxBuffer.Size > 0)
            CHECK(std::memcmp(copied.vertices.data(), live->VtxBuffer.Data,
                              sizeof(ImDrawVert) * static_cast<std::size_t>(live->VtxBuffer.Size)) == 0);
        if (live->IdxBuffer.Size > 0)
            CHECK(std::memcmp(copied.indices.data(), live->IdxBuffer.Data,
                              sizeof(ImDrawIdx) * static_cast<std::size_t>(live->IdxBuffer.Size)) == 0);
        for (std::size_t ci = 0; ci < copied.cmds.size(); ++ci)
        {
            const ImDrawCmd& liveCmd = live->CmdBuffer[static_cast<int>(ci)];
            const Arcane::ImDrawListSnapshot::Cmd& c = copied.cmds[ci];
            CHECK(c.elemCount == liveCmd.ElemCount);
            CHECK(c.idxOffset == liveCmd.IdxOffset);
            CHECK(c.vtxOffset == liveCmd.VtxOffset);
            CHECK(c.clip.x == liveCmd.ClipRect.x);
            CHECK(c.clip.y == liveCmd.ClipRect.y);
            CHECK(c.clip.z == liveCmd.ClipRect.z);
            CHECK(c.clip.w == liveCmd.ClipRect.w);
            CHECK(c.managed == liveCmd.TexRef._TexData);
            if (liveCmd.TexRef._TexData == nullptr)
            {
                CHECK(c.texture == liveCmd.TexRef._TexID);
                if (c.texture == static_cast<ImTextureID>(42))
                    saw42 = true;
                if (c.texture == static_cast<ImTextureID>(7))
                    saw7 = true;
            }
            else
                CHECK(c.texture == static_cast<ImTextureID>(0));
        }
    }
    CHECK(saw42);
    CHECK(saw7);

    Arcane::ImDrawSnapshotRebuild rebuilt(snap);
    ImDrawData* data = rebuilt.Data();
    REQUIRE(data->Valid);
    CHECK(data->CmdListsCount == src->CmdListsCount);
    REQUIRE(data->CmdLists.Size == src->CmdLists.Size);
    CHECK(data->TotalVtxCount == src->TotalVtxCount);
    CHECK(data->TotalIdxCount == src->TotalIdxCount);
    CHECK(data->DisplayPos.x == src->DisplayPos.x);
    CHECK(data->DisplayPos.y == src->DisplayPos.y);
    CHECK(data->DisplaySize.x == src->DisplaySize.x);
    CHECK(data->DisplaySize.y == src->DisplaySize.y);
    CHECK(data->FramebufferScale.x == src->FramebufferScale.x);
    CHECK(data->FramebufferScale.y == src->FramebufferScale.y);
    REQUIRE(data->Textures != nullptr);
    REQUIRE(data->Textures->Size == src->Textures->Size);
    // The vector is the rebuild's. The ImTextureData objects are the live ones.
    CHECK(data->Textures != src->Textures);
    for (int i = 0; i < src->Textures->Size; ++i)
        CHECK((*data->Textures)[i] == (*src->Textures)[i]);
    for (int li = 0; li < data->CmdLists.Size; ++li)
    {
        const ImDrawList* live = src->CmdLists[li];
        const ImDrawList* copy = data->CmdLists[li];
        REQUIRE(copy->VtxBuffer.Size == live->VtxBuffer.Size);
        REQUIRE(copy->IdxBuffer.Size == live->IdxBuffer.Size);
        REQUIRE(copy->CmdBuffer.Size == live->CmdBuffer.Size);
        if (live->VtxBuffer.Size > 0)
            CHECK(std::memcmp(copy->VtxBuffer.Data, live->VtxBuffer.Data,
                              sizeof(ImDrawVert) * static_cast<std::size_t>(live->VtxBuffer.Size)) == 0);
        if (live->IdxBuffer.Size > 0)
            CHECK(std::memcmp(copy->IdxBuffer.Data, live->IdxBuffer.Data,
                              sizeof(ImDrawIdx) * static_cast<std::size_t>(live->IdxBuffer.Size)) == 0);
        for (int ci = 0; ci < copy->CmdBuffer.Size; ++ci)
        {
            const ImDrawCmd& a = live->CmdBuffer[ci];
            const ImDrawCmd& b = copy->CmdBuffer[ci];
            CHECK(b.ElemCount == a.ElemCount);
            CHECK(b.IdxOffset == a.IdxOffset);
            CHECK(b.VtxOffset == a.VtxOffset);
            CHECK(b.UserCallback == nullptr);
            CHECK(b.ClipRect.x == a.ClipRect.x);
            CHECK(b.ClipRect.y == a.ClipRect.y);
            CHECK(b.ClipRect.z == a.ClipRect.z);
            CHECK(b.ClipRect.w == a.ClipRect.w);
            CHECK(b.TexRef._TexData == a.TexRef._TexData);
            CHECK(b.TexRef._TexID == a.TexRef._TexID);
            // GetTexID asserts when _TexData is set and TexID is still Invalid
            // (imgui.h:4080-4081). This CPU frame never uploads the atlas.
            if (a.TexRef._TexData == nullptr)
                CHECK(b.GetTexID() == a.GetTexID());
        }
    }

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(64.0f, 64.0f), ImGuiCond_Always);
    ImGui::Begin("callback");
    ImGui::GetWindowDrawList()->AddCallback(&DummyImGuiCallback, nullptr);
    ImGui::End();
    ImGui::Render();
    Arcane::ImDrawSnapshot refused;
    CHECK_FALSE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), refused));
    CHECK(refused.lists.empty());
    CHECK_FALSE(refused.hasTextures);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
}
```

- [ ] **Step 2: Build and run `[packet]`.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[packet]"
```

Expected: FAIL. `RenderFramePacket.hpp` does not exist.

- [ ] **Step 3: Implement the packet and the ring.**

`RenderFramePacket.hpp`:

```cpp
#pragma once

#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/GridSceneDesc.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/PostChainDesc.hpp>
#include <Arcane/Render/VisibleSet.hpp>
#include <Arcane/Scene/SceneCameraView.hpp>
#include <Arcane/Scene/ViewTransform.hpp>
#include <Arcane/Modules/ArcaneRenderApi.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    struct RenderOrigin
    {
        glm::vec3 translation{ 0.0f };
    };

    // Ruling PR-1: Render-owned plain data: no imgui type appears here, so
    // ArcaneRender never includes <imgui.h>. ArcaneImGui's ImDrawSnapshot.cpp fills
    // and rebuilds these and static_asserts that ImDrawSnapshotVert is ImDrawVert's
    // layout and that ImDrawIdx is 16-bit (ThirdParty/imgui/imconfig.h:121 leaves
    // the 32-bit override commented out).
    struct ImDrawSnapshotVert
    {
        glm::vec2     pos{ 0.0f };
        glm::vec2     uv{ 0.0f };
        std::uint32_t col = 0;
    };

    struct ImDrawListSnapshot
    {
        std::vector<ImDrawSnapshotVert> vertices;   // ImDrawVert bytes
        std::vector<std::uint16_t>      indices;    // ImDrawIdx
        struct Cmd
        {
            glm::vec4     clip{ 0.0f };           // ImDrawCmd::ClipRect
            std::uint64_t texture = 0;            // ImTextureID (ImU64), used only when managed == nullptr. ImTextureID_Invalid is 0.
            void*         managed = nullptr;      // TexRef._TexData (an ImTextureData*). Not owned.
            std::uint32_t idxOffset = 0;
            std::uint32_t elemCount = 0;
            std::uint32_t vtxOffset = 0;
        };
        std::vector<Cmd> cmds;
    };

    struct ImDrawSnapshot
    {
        std::vector<ImDrawListSnapshot> lists;
        std::vector<void*> textures;            // ImTextureData* pointer copy. Not owned.
        bool hasTextures = false;               // false when src->Textures was null, including an empty default ImDrawData
        glm::vec2 displayPos{ 0.0f };
        glm::vec2 displaySize{ 0.0f };
        glm::vec2 framebufferScale{ 1.0f };
    };

    struct RenderFramePacket
    {
        std::optional<SceneCameraView> spriteView;
        bool                           spriteViewFromScene = false;   // true only when a scene Camera produced spriteView
        std::optional<ViewTransform>   meshView;
        glm::uvec2                   viewport{ 0u, 0u };
        RenderOrigin                 origin{};

        GpuSceneFrame                mesh{};
        VisibleSet                   visible{};       // the main (sprite) view's set: SceneVisibility::views[0]
        VisibleSet                   meshVisible{};   // views[1]: the mesh view's set when it differs from the main view

        std::vector<Batch2DVertex>   batchVertices;
        std::vector<std::uint32_t>   batchIndices;
        std::vector<Batch2DDrawSpan> batchSpans;
        GlobalParams                 globals{};
        glm::mat4                    viewProjection{ 1.0f };

        std::vector<PickDrawable>    pickDrawables;
        OverlayPrimitiveList         overlay{};
        PostChainDesc                post{};
        bool                         hasPost = false;
        ImDrawSnapshot               imgui{};
        ImDrawSnapshot               gameUi{};
        std::vector<std::uint32_t>   selectedIds;

        glm::vec3 lightDirection{ 0.0f };
        glm::vec3 lightColor{ 0.0f };
        glm::vec3 ambient{ 0.0f };

        ARC_RENDER_API void Clear() noexcept;
    };

    class ARC_RENDER_API PacketRing
    {
    public:
        RenderFramePacket&       BeginExtract();
        RenderFramePacket&       Latest();
        const RenderFramePacket& Slot(std::uint32_t extractsAgo) const;

    private:
        std::array<RenderFramePacket, kMaxFramesInFlight> m_slots{};
        std::uint64_t m_extractCount = 0;
    };
}
```

Move `PostChainPassDesc` and `PostChainDesc` (`PostChainCache.hpp:56-79`) into a new `Source/Client/ArcaneRender/src/Arcane/Render/PostChainDesc.hpp` that does not include `Registry.hpp`. It is header-only (no export macro), forward-declares `class MaterialInstance;` and `class MaterialTemplate;` the way `PostChainCache.hpp:36-37` does, and includes `<cstdint>`, `<memory>`, and `<vector>`. `PostChainCache.hpp` includes that header. (`PostChainCache.hpp` itself is registry-free in this tree too, `:21-31`; the split keeps the `PostChainCache` class out of the packet.) `PickEmit.hpp` forward-declares `Astra::Registry` and does not include `Registry.hpp` (`PickEmit.hpp:36`); including it from the packet is safe. `GpuSceneTypes.hpp` includes `Astra/Entity/Entity.hpp` and does not include `Registry.hpp`.

`RenderFramePacket::Clear` clears every vector, resets both optionals, sets `spriteViewFromScene` and `hasPost` false, sets `origin.translation` to `{0,0,0}`, and default-constructs `mesh`, `visible`, `meshVisible`, `overlay`, `post`, `globals`, and the three light vectors. `meshVisible` and `spriteViewFromScene` exist so Task 6 can keep today's two-view cull and Task 7 today's no-camera behaviour (rev 5.2).

`PacketRing::BeginExtract`:

```cpp
RenderFramePacket& PacketRing::BeginExtract()
{
    const std::uint32_t depth = FramesInFlight();
    RenderFramePacket& slot = m_slots[m_extractCount % depth];
    slot.Clear();
    ++m_extractCount;
    return slot;
}

const RenderFramePacket& PacketRing::Slot(std::uint32_t extractsAgo) const
{
    const std::uint32_t depth = FramesInFlight();
    const std::uint64_t index = (m_extractCount - 1 - extractsAgo) % depth;
    return m_slots[static_cast<std::size_t>(index)];
}

RenderFramePacket& PacketRing::Latest()
{
    const std::uint32_t depth = FramesInFlight();
    return m_slots[static_cast<std::size_t>((m_extractCount - 1) % depth)];
}
```

`Latest()` is the slot `Slot(0)` names, non-const. Call it only after `BeginExtract` has run (`m_extractCount > 0`). `ExtractFrame` returns `const RenderFramePacket&` and does not write `selectedIds`. Spec 4.1 immutability holds for every other field. The only post-extract writers, both before the frame is recorded, are `ArmGraphViewportFrame`'s selection loop and the runtime `--pick-probe` stand-in. Both write `selectedIds` through `Latest()`. `Clear()` already clears that vector, so a slot starts empty.

`SceneCameraView.hpp`:

```cpp
#pragma once
#include <Arcane/Scene/ViewTransform.hpp>
#include <glm/vec2.hpp>
namespace Arcane
{
    struct SceneCameraView
    {
        glm::vec2 worldCenter{ 0.0f, 0.0f };
        float halfHeight = 0.0f;
        ViewTransform view{};
    };
}
```

`SceneCamera.hpp` includes that header beside `ViewTransform.hpp` and deletes the struct at `:31-42`. `ActiveSceneCamera` (`:57`) still returns `std::optional<SceneCameraView>`. `ActivePerspectiveSceneCamera` (`:206`) still returns `std::optional<ViewTransform>`.

Ruling PR-1: `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImDrawSnapshot.hpp` holds the two declarations that need imgui types. It includes `<Arcane/Modules/ArcaneImGuiApi.hpp>`, `<Arcane/Render/RenderFramePacket.hpp>`, `<imgui.h>`, and `<vector>`:

```cpp
namespace Arcane
{
    ARC_IMGUI_API bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out);

    class ARC_IMGUI_API ImDrawSnapshotRebuild
    {
    public:
        explicit ImDrawSnapshotRebuild(const ImDrawSnapshot& snap);
        ~ImDrawSnapshotRebuild();
        ImDrawData* Data() { return &m_data; }

        ImDrawSnapshotRebuild(const ImDrawSnapshotRebuild&) = delete;
        ImDrawSnapshotRebuild& operator=(const ImDrawSnapshotRebuild&) = delete;

    private:
        ImDrawData m_data{};
        ImVector<ImTextureData*> m_textures;
        std::vector<ImDrawList*> m_owned;
    };
}
```

`ImDrawSnapshot.cpp` pins the Render-owned types to imgui's, at namespace scope:

```cpp
static_assert(sizeof(Arcane::ImDrawSnapshotVert) == sizeof(ImDrawVert));
static_assert(offsetof(Arcane::ImDrawSnapshotVert, pos) == offsetof(ImDrawVert, pos));
static_assert(offsetof(Arcane::ImDrawSnapshotVert, uv) == offsetof(ImDrawVert, uv));
static_assert(offsetof(Arcane::ImDrawSnapshotVert, col) == offsetof(ImDrawVert, col));
static_assert(sizeof(ImDrawIdx) == sizeof(std::uint16_t));
static_assert(sizeof(ImTextureID) == sizeof(std::uint64_t));
```

`CopyImDrawSnapshot` and `ImDrawSnapshotRebuild`, in `ImDrawSnapshot.cpp` (rev 4.1 placed them in `RenderFramePacket.cpp`; the bodies are rev 4.1's, with the conversions the plain-data members need, marked `// PR-1`):

```cpp
bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out)
{
    out = {};
    if (!src)
        return true;
    out.displayPos = { src->DisplayPos.x, src->DisplayPos.y };
    out.displaySize = { src->DisplaySize.x, src->DisplaySize.y };
    out.framebufferScale = { src->FramebufferScale.x, src->FramebufferScale.y };
    if (src->Textures != nullptr)
    {
        out.hasTextures = true;
        out.textures.assign(src->Textures->Data, src->Textures->Data + src->Textures->Size);
    }
    out.lists.reserve(static_cast<std::size_t>(src->CmdLists.Size));
    for (const ImDrawList* list : src->CmdLists)
    {
        if (!list)
            continue;
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr)
            {
                out = {};
                return false;
            }
        }
        ImDrawListSnapshot snap;
        if (list->VtxBuffer.Size > 0)
        {
            snap.vertices.resize(static_cast<std::size_t>(list->VtxBuffer.Size));   // PR-1
            std::memcpy(snap.vertices.data(), list->VtxBuffer.Data,
                        sizeof(ImDrawVert) * static_cast<std::size_t>(list->VtxBuffer.Size));
        }
        if (list->IdxBuffer.Size > 0)
            snap.indices.assign(list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            ImDrawListSnapshot::Cmd c;
            c.clip = glm::vec4(cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w);   // PR-1
            c.idxOffset = cmd.IdxOffset;
            c.elemCount = cmd.ElemCount;
            c.vtxOffset = cmd.VtxOffset;
            // Do not call GetTexID here. A managed texture whose TexID is still
            // Invalid asserts (imgui.h:4073-4081). NewFrameTexUpdates stamps
            // that id later, at declaration.
            if (cmd.TexRef._TexData != nullptr)
            {
                c.managed = cmd.TexRef._TexData;
                c.texture = 0;
            }
            else
            {
                c.managed = nullptr;
                c.texture = cmd.TexRef._TexID;
            }
            snap.cmds.push_back(c);
        }
        out.lists.push_back(std::move(snap));
    }
    return true;
}

ImDrawSnapshotRebuild::ImDrawSnapshotRebuild(const ImDrawSnapshot& snap)
{
    m_data.Valid = true;
    m_data.DisplayPos = ImVec2(snap.displayPos.x, snap.displayPos.y);
    m_data.DisplaySize = ImVec2(snap.displaySize.x, snap.displaySize.y);
    m_data.FramebufferScale = ImVec2(snap.framebufferScale.x, snap.framebufferScale.y);
    m_data.OwnerViewport = nullptr;
    m_data.CmdLists.resize(0);
    m_data.CmdListsCount = 0;
    m_data.TotalVtxCount = 0;
    m_data.TotalIdxCount = 0;
    if (snap.hasTextures)
    {
        m_textures.resize(static_cast<int>(snap.textures.size()));
        for (int i = 0; i < m_textures.Size; ++i)
            m_textures[i] = static_cast<ImTextureData*>(snap.textures[static_cast<std::size_t>(i)]);   // PR-1
        m_data.Textures = &m_textures;
    }
    else
        m_data.Textures = nullptr;
    for (const ImDrawListSnapshot& src : snap.lists)
    {
        ImDrawList* list = IM_NEW(ImDrawList(nullptr));
        if (!src.vertices.empty())
        {
            list->VtxBuffer.resize(static_cast<int>(src.vertices.size()));
            std::memcpy(list->VtxBuffer.Data, src.vertices.data(),
                        src.vertices.size() * sizeof(ImDrawVert));   // layout pinned by the static_asserts above
        }
        if (!src.indices.empty())
        {
            list->IdxBuffer.resize(static_cast<int>(src.indices.size()));
            std::memcpy(list->IdxBuffer.Data, src.indices.data(),
                        src.indices.size() * sizeof(ImDrawIdx));
        }
        for (const ImDrawListSnapshot::Cmd& c : src.cmds)
        {
            ImDrawCmd cmd{};
            cmd.ClipRect = ImVec4(c.clip.x, c.clip.y, c.clip.z, c.clip.w);   // PR-1
            if (c.managed != nullptr)
            {
                cmd.TexRef._TexData = static_cast<ImTextureData*>(c.managed);   // PR-1
                cmd.TexRef._TexID = ImTextureID_Invalid;
            }
            else
                cmd.TexRef = ImTextureRef(c.texture);
            cmd.IdxOffset = c.idxOffset;
            cmd.ElemCount = c.elemCount;
            cmd.VtxOffset = c.vtxOffset;
            cmd.UserCallback = nullptr;
            list->CmdBuffer.push_back(cmd);
        }
        m_owned.push_back(list);
        m_data.CmdLists.push_back(list);
        m_data.CmdListsCount += 1;
        m_data.TotalVtxCount += list->VtxBuffer.Size;
        m_data.TotalIdxCount += list->IdxBuffer.Size;
    }
}

ImDrawSnapshotRebuild::~ImDrawSnapshotRebuild()
{
    for (ImDrawList* list : m_owned)
        IM_DELETE(list);
}
```

`ImDrawSnapshot.cpp` includes `<Arcane/ImGui/ImDrawSnapshot.hpp>`, `<cstddef>` (`offsetof`), and `<cstring>` for the `memcpy` calls; `RenderFramePacket.cpp` holds only `RenderFramePacket::Clear` and the `PacketRing` bodies. The hook (ruling PR-1): `RenderOverlayHooks` (`RenderOverlayNode.hpp:29-33`) gains a third member after `addNode`, `bool (*snapshot)(const ImDrawData* src, ImDrawSnapshot& out) = nullptr;`, with `struct ImDrawSnapshot;` forward-declared in `namespace Arcane` beside `class NriGraphContext;` (`:15`). `InstallImGuiHostHooks` (`ImGuiHostHooks.cpp:17-22`) passes `&CopyImDrawSnapshot` as that third initializer and includes `<Arcane/ImGui/ImDrawSnapshot.hpp>`. The existing two-member brace init is otherwise unchanged. The destructor `IM_DELETE`s the owned lists and does not delete the `ImTextureData` pointers. `m_textures` outlives `Data()` for as long as the rebuild object does.

`ImGuiNri::RenderDrawData` still asserts `UserCallback == nullptr` (`ImGuiNri.cpp:954`). The copy refuses before a callback can be stored, which is what the packet test checks. Task 10 builds one stack `ImDrawSnapshotRebuild` in the declaration lambda and another in the record lambda. Both name the same `ImTextureData` objects. `PrepareFrame` is what calls `NewFrameTexUpdates`; `Record` passes `Data()` to `RenderDrawData`. The rebuild is not stored on the node.

Append to `NriGraphPixelTest.cpp`, after `CheckImGuiDrawsPastOnePool`. The helpers `MakeVehicle`, `RenderOne`, `At`, `kW`, and `kH` are file-local and already visible to later cases in that TU. Add `#include <Arcane/Render/RenderFramePacket.hpp>` and `#include <Arcane/ImGui/ImDrawSnapshot.hpp>` (the file already includes `<Arcane/ImGui/ImGuiNriNode.hpp>` at `:1`). `ImGuiHud()` returns the `RenderOverlayNode*` seam (`NriGraphContext.hpp:1041`), which declares `AdoptImGuiContext(void*)` (`RenderOverlayNode.hpp:22`). The case does not `BindPacket`, so it stays on `FrameDesc::imgui` after Task 10's packet branch. `NodeSet::hostHud` must be set or `ImGuiHud()` is null on an offscreen context (`NriGraphContext.hpp:360-367`). After Task 2, `FrameDesc::world` defaults true, so the frame also declares an empty `batch2d`; the rect still composites after tonemap. `AddRectFilled` samples the font atlas. `NewFrameTexUpdates` no-ops when `Textures` is null (`ImGuiNri.cpp:775-776`), the atlas `TexID` stays invalid, and the unregistered guard (`:980-991`) drops the draw. The red pixel is the witness that the rebuild carried `Textures` and that declaration serviced them before record. Colour is structural, the same rule as the rest of this file: display-referred, not an exact ACES byte.

```cpp
namespace
{
    void CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const imgui = ImGui::CreateContext();
        REQUIRE(imgui != nullptr);
        ImGui::SetCurrentContext(imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.DeltaTime = 1.0f / 60.0f;

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.hostHud = true;
        PixelVehicle v = MakeVehicle(backend, nodes);
        Arcane::RenderOverlayNode* const hud = v.ctx->ImGuiHud();
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(imgui);

        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(70.0f, 40.0f), ImVec2(90.0f, 56.0f), IM_COL32(255, 0, 0, 255));
        ImGui::Render();

        Arcane::ImDrawSnapshot snap;
        REQUIRE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), snap));
        REQUIRE(snap.hasTextures);
        Arcane::ImDrawSnapshotRebuild rebuilt(snap);
        REQUIRE(rebuilt.Data()->Textures != nullptr);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.imgui = rebuilt.Data();
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        REQUIRE(w == kW);
        REQUIRE(h == kH);
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.r > px.g + 60);
        CHECK(px.r > px.b + 60);
        CHECK(Arcane::RenderErrorCount() == before);

        v.ctx.reset();
        ImGui::DestroyContext(imgui);
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("an ImDrawSnapshot rebuild reaches ImGuiNri (d3d12)",
          "[gpu][packet][imgui][nri][d3d12]")
{
    CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("an ImDrawSnapshot rebuild reaches ImGuiNri (vulkan)",
          "[gpu][packet][imgui][nri][vulkan]")
{
    CheckImGuiSnapshotReachesRenderer(Arcane::GraphicsBackend::Vulkan);
}
```

Call `LatchFramesInFlight()` before the first `FramesInFlight()` use if it has not been latched; the test does that. The ring does not allocate `nri::Buffer` or `nri::Device` members.

A comment on `batchVertices` states the contract: `Batch2DDrained`'s spans die at the next `Begin()` (`Batcher2D.hpp:160-163`); the packet owns the copy. Task 5 is what performs that copy. This task does not change hosts.

Sprite rows from spec section 6 are not a field. Plans 1 and 2 carry world sprites as `batchVertices` / `batchIndices` / `batchSpans`.

- [ ] **Step 4: Run `[packet]`.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[packet]"
```

Expected: PASS. The CPU case matches every list, including one command with `VtxOffset > 0`, and the callback frame returns false. The two `[gpu][packet]` cases read a red-dominant pixel at `(80, 48)` and leave `RenderErrorCount` unchanged. Record the seed.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCameraView.hpp Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCamera.hpp Source/Client/ArcaneRender/src/Arcane/Render/VisibleSet.hpp Source/Client/ArcaneRender/src/Arcane/Render/VisibilitySystem.hpp Source/Client/ArcaneRender/src/Arcane/Render/PostChainDesc.hpp Source/Client/ArcaneRender/src/Arcane/Render/PostChainCache.hpp Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.hpp Source/Client/ArcaneRender/src/Arcane/Render/RenderFramePacket.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/RenderOverlayNode.hpp Source/Client/ArcaneImGui/src/Arcane/ImGui/ImDrawSnapshot.hpp Source/Client/ArcaneImGui/src/Arcane/ImGui/ImDrawSnapshot.cpp Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiHostHooks.cpp Tests/ArcaneTests/src/RenderFramePacketTest.cpp Tests/ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): RenderFramePacket ring sized to FramesInFlight" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 5: Sprites and the batch copy

**Files:**
- Create: `Source/Runtime/ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp`
- Create: `Source/Runtime/ArcaneCore/src/Arcane/Base/ExtractAccounting.cpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp`
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.hpp` (`FrameIo` at `:71`, add `PacketRing` as the last member)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp:521-552` and the branch at `:753`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp` (beside `CurrentBatch()` at `:1127`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp` (clear the bound pointer at the end of `RenderFrame` and `RenderFrameOffscreen`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1618-1641`
- Modify: `Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.cpp:47-51`
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorApp.hpp` (a `PacketRing` member on `EditorApp`, class at `:116`, and `Arcane::ExtractRequest BuildExtractRequest(std::uint32_t w, std::uint32_t h)`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorApp.cpp` (`CaptureGraphViewportPng`, `:2056-2090`: the scene-save cover extracts and binds a packet too)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` `RenderSceneToViewport` (`:1651`, editor view already computed at `:1694-1697`)
- Modify: `Tests/ArcaneTests/src/ClientRuntimeTest.cpp:38-47`
- Modify: `Tests/ArcaneTests/src/NriGraphPixelTest.cpp` (append the registered-material witness)
- Test: `Tests/ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `PacketRing::BeginExtract` from Task 4. `Batch2DDrained` (`Batcher2D.hpp:164-189`). `RenderContext2D` (`SceneResources.hpp:112-125`) and `ClientRuntime::SetRenderContext` (`ClientRuntime.cpp:125-133`), which is what writes that resource. `RenderSubmissionSystem::operator()` (`RenderSystems.hpp:50` through its closing brace) returns immediately when `ctx->batcher` is null (`:52-53`) and submits a resolved material through `QuadWorld` (`:230-240`). `QuadWorld` rewrites an id that is `>= m_materials.size()` to `kMaterialSprite` (`Batcher2D.cpp:248-253`). `RegisterMaterial` appends and returns `size - 1`, so the first registered id is 3 (`Batcher2D.cpp:96-102`, `Batcher2D.hpp:210-216`). `Begin` clears geometry and resets the view-projection to identity (`Batcher2D.cpp:139-142`). `SetViewProjection` (`Batcher2D.hpp:422`) does not clear geometry. `SpriteMaterialCache::ConsumeResult` registers on the batcher it is given (`SpriteMaterialCache.cpp:260`), and the host passes its own batcher (`SceneRenderResolver.cpp:581`). `ActiveSceneCamera` returns `std::optional<SceneCameraView>` (`SceneCamera.hpp:57`). `ViewTransform::ViewProjection`. `Batch2DNode::Prepare` (`Batch2DNode.hpp:226`) looks up `MaterialDesc` on the batcher it is passed (`Batch2DNode.cpp:1170`); a missing pipeline draws through the plain sprite pipeline (`:1247`, `:1342-1345`). `RenderFrameOffscreen` sets `m_currentBatch = effective.batch` (`NriGraphContext.cpp:1917`). `FrameIo` designated init at `RuntimeApp.cpp:748` (a new member with a default constructor needs no new initializer). Extract does not call `Batcher2D::Create`.
- Produces the stable request and the one extract entry. Later tasks fill fields of this same struct. They do not add members. `physicsSelectedBody` is editor state (the primary selection, an entity, not a body handle), so filling it is not a registry read in a frame builder; Task 9 is the task that resolves it to a body inside extract.

```cpp
struct ExtractRequest
{
    glm::uvec2                   viewport{ 0u, 0u };
    std::optional<ViewTransform> editorView;
    bool                         playMode = false;
    bool                         showGrid2d = false;
    bool                         showGrid3d = false;
    bool                         perspective = false;
    GridSceneDesc                grid3d{};
    float                        gridMinorAlpha = 0.35f;
    float                        gridMajorAlpha = 0.55f;
    glm::vec3                    gridLineRgb{ 0.5f, 0.5f, 0.5f };
    float                        gridLineThicknessPx = 1.0f;
    std::int64_t                 gridMaxLinesPerAxis = 4096;
    glm::vec4                    gridAxisX{ 0.85f, 0.25f, 0.25f, 0.9f };
    glm::vec4                    gridAxisY{ 0.3f, 0.8f, 0.3f, 0.9f };
    bool                         physicsDebug = false;
    bool                         physicsWholeWorld = false;
    std::optional<Astra::Entity> physicsSelectedBody;   // the editor's primary selection; Task 9 maps it to its body
    bool                         cameraRect = false;
    glm::vec4                    cameraFrameColor{ 0.45f, 0.62f, 0.78f, 0.75f };
    OverlayPrimitiveList         gizmoPrims{};
    const PostChainDesc*         post = nullptr;
    const ImDrawData*            imgui = nullptr;    // copied in Task 10; the pointer is not stored
    const ImDrawData*            gameUi = nullptr;
    glm::vec3                    lightDirection{ 0.0f };
    glm::vec3                    lightColor{ 0.0f };
    glm::vec3                    ambient{ 0.0f };
    GpuScene*                    device = nullptr;
};

ARC_RENDER_API const RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring);

// ExtractAccounting.hpp (ArcaneCore, <Arcane/Core/Api.hpp>):
ARC_CORE_API void NoteExtractFrameCall() noexcept;
ARC_CORE_API std::uint64_t ExtractFrameCallCount() noexcept;
ARC_CORE_API void ResetExtractFrameCallCount() noexcept;
```

`ExtractRequest` and `ExtractFrame` are in `ExtractFrame.hpp` (ArcaneRender), which includes `<Arcane/Modules/ArcaneRenderApi.hpp>`; the three counter functions are in `ExtractAccounting.hpp` (ArcaneCore), which includes `<Arcane/Core/Api.hpp>`, so ArcaneServer reaches them without a Client module. `ExtractFrame.hpp` forward-declares `namespace Astra { class Registry; }`, `class GpuScene`, and, at global scope, `struct ImDrawData;` (the way `NriGraphContext.hpp:264` does; `RenderFramePacket.hpp` no longer brings `<imgui.h>`, (ruling PR-1)). It includes `RenderFramePacket.hpp`. It does not include `Astra/Registry/Registry.hpp` or `SceneCamera.hpp`. `ExtractFrame.cpp` is the one `.cpp` under `Source/Client/ArcaneRender/src/Arcane/Render/` that includes `Registry.hpp`.

`NriGraphContext` gains:

```cpp
void BindPacket(const RenderFramePacket* packet) noexcept;
const RenderFramePacket* Packet() const noexcept;
```

- [ ] **Step 1: Write the failing test.**

Create `Tests/ArcaneTests/src/ExtractFrameTest.cpp`. Copy `struct World` from `GpuSceneSyncTest.cpp:32-105` into this file's anonymous namespace (that copy is not visible to other TUs). Add these methods inside `World`:

```cpp
Astra::Entity sprite{};
Astra::Entity AddSprite(glm::vec3 pos)
{
    sprite = reg.CreateEntity();
    Arcane::Transform t;
    t.position = pos;
    reg.AddComponent<Arcane::Transform>(sprite, t);
    reg.SetParent(sprite, root);
    reg.AddComponent<Arcane::SpriteRenderer>(sprite, Arcane::SpriteRenderer{});
    return sprite;
}
```

`WorldTransform` is a `glm::mat4` (`Components.hpp:87-91`). `SpriteRenderer::shape` defaults to `Rect` (`Components.hpp:152`). `GetComponent` returns a pointer. An unresolved Rect sprite is a 1x1 m quad: `Batcher2D.cpp:437-440` pushes four vertices.

`RenderSubmissionSystem` submits nothing when `RenderContext2D::batcher` is null (`RenderSystems.hpp:52-53`). The test installs the host batcher and `Begin`s it before `ExtractFrame`. Extract does not `Begin`. `editorView` stays so Task 6's visible set still contains the origin quad.

```cpp
TEST_CASE("extract copies one sprite quad and ignores a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    // Task 6's BuildVisibleSet only inserts entities that already have
    // WorldBounds (VisibilitySystem.hpp:124-128). RenderSubmissionSystem
    // drops anything that set does not contain (RenderSystems.hpp:62-63).
    Arcane::BoundsSystem{}(world.reg);

    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(ctx);

    Arcane::ResetExtractFrameCallCount();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    // Ortho view so Task 6's visible set still contains this quad.
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    CHECK(packet.origin.translation == glm::vec3(0.0f));
    CHECK(packet.viewport == req.viewport);
    CHECK(packet.batchVertices.size() == 4u);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
    const glm::vec3 copied = packet.batchVertices[0].pos;

    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 50.0f, 50.0f, 0.0f };
    CHECK(packet.batchVertices[0].pos == copied);
    CHECK(Arcane::ExtractFrameCallCount() == 1u);
}
```

The registered-id case uses the Severance bytes-only registration (`SeveranceTest.cpp:215-237`): non-empty `vsBytes` / `psBytes`, a template, and an instance. `RegisterMaterial` refuses empty bytes (`Batcher2D.cpp:469-475`). This case does not compile a shader and does not draw. The `SpriteMaterialTable` map outlives the `ExtractFrame` call. `editorView` is the same ortho view, so Task 6 does not cull the origin sprite.

```cpp
TEST_CASE("extract keeps a registered sprite material id", "[nri][extract]")
{
    World world;
    auto batcher = Arcane::Batcher2D::Create();
    REQUIRE(batcher != nullptr);

    auto templ = std::make_shared<Arcane::MaterialTemplate>(Arcane::MaterialTemplate::Build(
        "extract-id", 1, {}));
    auto instance = std::make_shared<Arcane::MaterialInstance>(
        std::shared_ptr<const Arcane::MaterialTemplate>(templ));
    Arcane::Material2DDesc desc;
    desc.templ = templ;
    desc.instance = instance;
    desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 5, 6, 7, 8 });
    const std::uint16_t id = batcher->RegisterMaterial(desc);
    REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
    REQUIRE(id >= 3u);
    REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
    REQUIRE(batcher->MaterialDesc(id) != nullptr);

    batcher->Begin(128u, 128u);
    Arcane::RenderContext2D ctx;
    ctx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(ctx);

    const Arcane::Guid matGuid = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, std::uint16_t> ids;
    ids.emplace(matGuid, id);
    world.reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

    Astra::Entity sprite = world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::SpriteRenderer& rend = *world.reg.GetComponent<Arcane::SpriteRenderer>(sprite);
    rend.material = matGuid;
    rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.batchSpans.size() == 1u);
    CHECK(packet.batchSpans[0].material == id);
    CHECK(packet.batchSpans[0].material != Arcane::Batcher2D::kMaterialSprite);
    CHECK(packet.batchSpans[0].worldSpace);
    CHECK(packet.batchVertices.size() == 4u);
}
```

`MaterialTemplate::Build` takes `std::vector<ParamDecl>` (`MaterialTemplate.hpp:38-39`). An empty decl list is a valid layout (`CbSize() == 0`). Include `MaterialTemplate.hpp`, `MaterialInstance.hpp`, `Batcher2D.hpp`, `SceneResources.hpp`, `BoundsSystem.hpp`, `<memory>`, and `<unordered_map>` from `ExtractFrameTest.cpp`. `BuildVisibleSet` only inserts entities that already carry `WorldBounds` (`VisibilitySystem.hpp:124-128`), and once that set exists `RenderSubmissionSystem` drops every other sprite (`RenderSystems.hpp:62-63`). `BoundsSystem` is what writes the box (`BoundsSystem.hpp:150`). The three sprite cases call it after transform propagation so they still submit after Task 6.

The pixel witness is appended to `NriGraphPixelTest.cpp`. Add these includes: `ExtractFrame.hpp`, `MaterialSource.hpp`, `MaterialInstance.hpp`, `ShaderSourceProvider.hpp`, `ShaderCompiler.hpp`, `ShaderConventions.hpp`. `MakeVehicle`, `RenderOne`, `At`, `kW`, and `kH` stay the file-local helpers. The snippet is `float4 shade(Varyings v) { return float4(0.0, 1.0, 0.0, 1.0); }` stitched into `materials/sprite_material.hlsl` (`MaterialTemplateFile(MaterialSurface::Sprite)` returns that name, `MaterialSource.cpp:334-335`; the shade slot is the comment at `data/shaders/materials/sprite_material.hlsl:9`). `BuildMaterialShaderSource` returns `MaterialBuildResult` (`MaterialSource.hpp:118-124`); check `errors.empty()`. There is no `Ok()`. `ShaderCompiler::CompileNow` returns both `dxil` and `spirv` (`ShaderCompiler.hpp:89-90`); do not pass SPIR-V arguments on the request. Pick `spirv` when `backend == GraphicsBackend::Vulkan`, otherwise `dxil`. `Material2DDesc` stores one target (`Batcher2D.hpp:61-69`). A registration whose pipeline will not build falls back to `sprite.hlsl` `ps_main` (`data/shaders/sprite.hlsl:68-70`, `return g_Texture.Sample(g_Sampler, input.uv) * input.color`), which with a red tint is red-dominant after ACES. The constant green is green-dominant. `CHECK(px.g > px.r + 60)` is that difference. Those pixels would change if `QuadWorld` rewrote the id to `kMaterialSprite`.

The sprite sits at world `(80, 48)` with scale `{16, 16, 1}`. Unresolved rects are 1x1 m about the centre pivot (`RenderSystems.hpp:156-157`), so the quad covers about 16 px and the centre sample is inside it. `editorView` is `Ortho2DView(vec2(kW * 0.5, kH * 0.5), kH * 0.5, {kW, kH})`: 1 px/m, the same mapping `NriGraphPixelTest.cpp:432-436` states. Task 6's visible set keeps the sprite because that view contains it. Task 7's view-projection comes from `packet.spriteView`, which extract fills from `editorView`. `frame.batch` is the same batcher the material was registered on, because `Prepare` resolves `MaterialDesc` on `CurrentBatch()` and `RenderFrameOffscreen` sets that from `frame.batch` (`NriGraphContext.cpp:1917`). `BindPacket` is required or the node drains the live batcher again.

```cpp
namespace
{
    void CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get(
            Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Sprite));
        REQUIRE(templateText.has_value());
        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText,
            "float4 shade(Varyings v) { return float4(0.0, 1.0, 0.0, 1.0); }",
            "extract_green",
            Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest compileReq;
            compileReq.debugName = "extract_green.hlsl";
            compileReq.sourceUtf8 = build.hlsl;
            compileReq.entry = entry;
            compileReq.profile = profile;
            return compiler.CompileNow(compileReq);
        };
        const Arcane::ShaderCompileResult vs = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        const Arcane::ShaderCompileResult ps = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        const bool vulkan = backend == Arcane::GraphicsBackend::Vulkan;
        const Arcane::ShaderTargetResult& vsTarget = vulkan ? vs.spirv : vs.dxil;
        const Arcane::ShaderTargetResult& psTarget = vulkan ? ps.spirv : ps.dxil;
        REQUIRE(vsTarget.succeeded);
        REQUIRE(psTarget.succeeded);

        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);
        auto templ = std::make_shared<const Arcane::MaterialTemplate>(build.templ);
        auto instance = std::make_shared<const Arcane::MaterialInstance>(templ);
        Arcane::Material2DDesc desc;
        desc.templ = templ;
        desc.instance = instance;
        desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(vsTarget.bytecode);
        desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(psTarget.bytecode);
        const std::uint16_t id = batcher->RegisterMaterial(std::move(desc));
        REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
        REQUIRE(id != Arcane::Batcher2D::kMaterialSprite);
        batcher->Begin(kW, kH);

        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        Arcane::RegisterSceneComponents(reg);
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });

        Arcane::RenderContext2D renderCtx;
        renderCtx.batcher = batcher.get();
        reg.SetResource<Arcane::RenderContext2D>(renderCtx);
        const Arcane::Guid matGuid = Arcane::Guid::Generate();
        std::unordered_map<Arcane::Guid, std::uint16_t> ids;
        ids.emplace(matGuid, id);
        reg.SetResource<Arcane::SpriteMaterialTable>(Arcane::SpriteMaterialTable{ &ids });

        Astra::Entity sprite = reg.CreateEntity();
        Arcane::Transform xform;
        xform.position = { 80.0f, 48.0f, 0.0f };
        xform.scale = { 16.0f, 16.0f, 1.0f };
        reg.AddComponent<Arcane::Transform>(sprite, xform);
        reg.SetParent(sprite, root);
        Arcane::SpriteRenderer rend;
        rend.tint = { 1.0f, 0.0f, 0.0f, 1.0f };
        rend.material = matGuid;
        reg.AddComponent<Arcane::SpriteRenderer>(sprite, rend);
        Arcane::TransformPropagationSystem{}(reg);
        Arcane::BoundsSystem{}(reg);

        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { kW, kH };
        req.editorView = Arcane::Ortho2DView(
            glm::vec2(static_cast<float>(kW) * 0.5f, static_cast<float>(kH) * 0.5f),
            static_cast<float>(kH) * 0.5f,
            req.viewport);
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(reg, req, ring);
        REQUIRE(packet.batchSpans.size() == 1u);
        CHECK(packet.batchSpans[0].material == id);
        CHECK(packet.batchSpans[0].worldSpace);

        PixelVehicle v = MakeVehicle(backend);
        v.ctx->BindPacket(&packet);
        Arcane::NriGraphContext::FrameDesc frame;
        frame.batch = batcher.get();
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.g > px.r + 60);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("extract keeps a registered sprite material on screen (d3d12)",
          "[gpu][extract][shadercompile][nri][d3d12]")
{
    CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("extract keeps a registered sprite material on screen (vulkan)",
          "[gpu][extract][shadercompile][nri][vulkan]")
{
    CheckExtractKeepsRegisteredMaterial(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Build and run `[extract]`.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
```

Expected: FAIL. `ExtractFrame` does not exist yet.

- [ ] **Step 3: Implement the counter, the sprite copy, and the two host call sites.**

`ExtractAccounting.hpp` declares the three functions in `namespace Arcane`. `ExtractAccounting.cpp`:

```cpp
#include <Arcane/Base/ExtractAccounting.hpp>
#include <atomic>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        std::atomic<std::uint64_t> g_extractFrameCalls{ 0 };
    }

    void NoteExtractFrameCall() noexcept
    {
        g_extractFrameCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t ExtractFrameCallCount() noexcept
    {
        return g_extractFrameCalls.load(std::memory_order_relaxed);
    }

    void ResetExtractFrameCallCount() noexcept
    {
        g_extractFrameCalls.store(0, std::memory_order_relaxed);
    }
}
```

`ExtractFrame` begins with `NoteExtractFrameCall()`, then `RenderFramePacket& packet = ring.BeginExtract()`. It sets `packet.viewport = req.viewport` and `packet.origin.translation = { 0.0f, 0.0f, 0.0f }`. It does not write `packet.spriteView` or `packet.meshView` in this task.

Sprite copy, local to `ExtractFrame.cpp`. There is no `Batcher2D::Create`, no `Begin`, and no `SetGlobals`. `Begin` would clear the host's geometry and reset its view-projection (`Batcher2D.cpp:139-142`). `SetGlobals(GlobalParams{})` would wipe the host's `Time` / `DeltaTime`. The host already called `Begin`, `SetGlobals`, and `SetRenderContext` on its persistent batcher. Registration lives on that batcher (`SceneRenderResolver.cpp:581`, `SpriteMaterialCache.cpp:260`). `QuadWorld` on a fresh batcher rewrites any id `>= m_materials.size()` to `kMaterialSprite` (`Batcher2D.cpp:248-253`), and a `Create()` only has the three built-ins (`:88-94`). Extract does not replace `RenderContext2D::batcher`. When that pointer is null the sprite vectors stay empty and nothing is drained.

```cpp
RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
Batcher2D* batcher = ctx ? ctx->batcher : nullptr;
if (batcher)
{
    glm::mat4 vp{ 1.0f };
    if (req.editorView)
        vp = req.editorView->ViewProjection();
    else if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
        vp = cam->view.ViewProjection();
    else
        vp = ctx->view.ViewProjection();   // no scene camera: the host's stored view (a plugin's SetView), never identity
    batcher->SetViewProjection(vp);   // does not clear geometry
    RenderSubmissionSystem{}(reg);
    const Batch2DDrained drained = batcher->Drain();
    packet.batchVertices.assign(drained.vertices.begin(), drained.vertices.end());
    packet.batchIndices.assign(drained.indices.begin(), drained.indices.end());
    packet.batchSpans.assign(drained.spans.begin(), drained.spans.end());
    if (drained.globals)
        packet.globals = *drained.globals;
    packet.viewProjection = drained.viewProjection;
}
return packet;
```

`ClientRuntime::InstallRenderSystems` (`ClientRuntime.cpp:47-51`) stops calling `render.AddSystem<RenderSubmissionSystem>()`. The function stays, so `OnSystemsCleared` still compiles. The type stays, so a test that constructs `RenderSubmissionSystem` still compiles. In `ClientRuntimeTest.cpp`, rename the second case to `"ClientRuntime owns a Runtime, attaches as its client, and leaves render submission uninstalled"`, and change both `CHECK(...HasSystem<RenderSubmissionSystem>())` assertions on `ClientRuntime` (lines 44 and 46) to `CHECK_FALSE`. Leave the bare-`Runtime` `CHECK_FALSE` assertions as they are.

`FrameIo` gains `Arcane::PacketRing packetRing{};` as its last member. `NriGraphContext` stores `const RenderFramePacket* m_packet = nullptr;`, `BindPacket` assigns it, and `Packet()` returns it. Both `RenderFrame` and `RenderFrameOffscreen` set it back to null before they return.

`RuntimeFrame.cpp` keeps `Batch().Begin` (`:523`), `SetGlobals` (`:524`), `PushSceneCamera` (`:525`), and `SetViewProjection` (`:531`). Replace the block at `:548-552` with `SetRenderContext` only. `SubmitRender` goes away here so the sprite system does not run twice; `InstallRenderSystems` also stops adding it, above. The perf bracket that only timed `SubmitRender` goes with it.

```cpp
io.runtime->SetRenderContext(&io.gpu->Batch());
```

That call is still before the one `ExtractFrame`, which sits immediately above the branch at `:753`, after `graphFrame.imgui` (`:510`) and `graphFrame.post` (`:697`) have been assigned:

```cpp
Arcane::ExtractRequest req;
req.viewport = { frameWidth, frameHeight };
req.device = io.graph->Scene();
const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(io.runtime->Registry(), req, io.packetRing);
io.graph->BindPacket(&packet);
```

`graphFrame.batch = &io.gpu->Batch()` (`:690`) stays, so `Prepare` resolves materials on the same batcher extract drained. Leave `ActivePerspectiveSceneCamera` and `PrepareSceneForRender` in this task. `PushSceneCamera` still calls `ActiveSceneCamera`. There is exactly one `ExtractFrame(` in the file, textually above both `RenderFrame(` and `RenderFrameOffscreen(`. `GpuSceneInvalidate` at `:761` stays. The `Batch().Stats()` reads at `:323` and `:796` use the live batcher, which no longer receives the sprite system. Point them at the packet: draw count is `packet.batchSpans.size()`, quad count is `packet.batchVertices.size() / 4`.

`AddBatch2DNode` (`Batch2DNode.cpp:1628-1641`):

```cpp
Batch2DDrained drained;
if (context && context->Packet())
{
    const RenderFramePacket* packet = context->Packet();
    drained.vertices = packet->batchVertices;
    drained.indices = packet->batchIndices;
    drained.spans = packet->batchSpans;
    drained.globals = &packet->globals;
    drained.viewProjection = packet->viewProjection;
    drained.viewport = glm::vec2(float(width), float(height));
    if (Batcher2D* batcher = context->CurrentBatch())
        if (Batch2DNode* node = context->Batch2D())
            node->Prepare(*batcher, drained, canvasFormat);
}
else if (context)
{
    if (Batcher2D* batcher = context->CurrentBatch())
    {
        drained = batcher->Drain();
        if (Batch2DNode* node = context->Batch2D())
            node->Prepare(*batcher, drained, canvasFormat);
    }
}
```

The spans view the packet vectors. Those vectors live until `FramesInFlight()` later extracts recycle the slot. The packet path does not `Drain` again. `Prepare` still takes `CurrentBatch()`, which the hosts set to the same batcher extract submitted into (`RuntimeFrame.cpp:690`, `EditorAppFrame.cpp:1713`, `NriGraphContext.cpp:1917`). A material id in the span is only drawable when that batcher's `MaterialDesc` still has the registration. A graph with no bound packet, including thumbnail harvest and shader preview, keeps the `Drain()` path. Those two do not gain an `ExtractFrame` call.

`EditorApp` holds `Arcane::PacketRing m_packetRing;`. Inside `RenderSceneToViewport`, when `m_viewportTargets.graph` is set, keep `Begin` (`:1702`) and `SubmitSceneToBatcher`. Delete only `m_runtime->Loop().SubmitRender()` at `:1900`. `SetRenderContext(&b)` at `:1899` stays, and `SetGlobals` / `SetViewProjection` inside `SubmitSceneToBatcher` stay. Call `ExtractFrame` once after `SubmitSceneToBatcher` returns, not after the view block at `:1693-1697` (that block is before `Begin`, so a drain there would copy the previous bracket). If `!InPlayMode()`, `req.editorView = m_runtime->View()`; `req.viewport` is `{ w, h }`; `req.device` is `m_viewportTargets.graph->Scene()`. `m_viewportTargets.graph->BindPacket(&packet)` for the viewport frame. `vp.batch = &b` (`:1713`) stays, the same batcher. `BindPacket(nullptr)` runs when that frame returns. Task 3 already moved grid, physics, the camera rect, and the gizmo off this batcher, so the drain is sprites. This task does not remove `PrepareSceneForRender` or `ActivePerspectiveSceneCamera` from the editor. Task 9's later "stops calling `SubmitRender`" finds the call already gone.

The request comes from one helper, `EditorApp::BuildExtractRequest(w, h)` (in `EditorAppFrame.cpp`), so the viewport frame and the scene-save cover ask for the same picture. It fills only editor state and settings, never the registry. Later tasks that say the editor's `ExtractFrame` call "sets" a request field mean this helper sets it.

The scene-save cover is a second frame builder that renders through `SubmitSceneToBatcher` with no extract: `CaptureGraphViewportPng` (`EditorApp.cpp:2056-2090`) does `b.Begin`, `SubmitSceneToBatcher(b)`, `RenderFrameOffscreen(vp)`. With `SubmitRender` gone from `SubmitSceneToBatcher` and `RenderSubmissionSystem` uninstalled, its batcher drains empty and the cover loses its sprites. After `SubmitSceneToBatcher(b)` (`:2068`), it builds `Arcane::ExtractRequest req = BuildExtractRequest(graph->SurfaceWidth(), graph->SurfaceHeight());`, clears `req.gizmoPrims` (today's cover has no gizmo: the gizmo is ImGui foreground chrome, `EditorAppFrame.cpp:3717-3741`; from Task 9 the request builder fills that field), calls `ExtractFrame(m_runtime->Registry(), req, m_packetRing)`, and calls `graph->BindPacket(&packet)` immediately before `RenderFrameOffscreen(vp)` (`:2078`); the vehicle nulls the pointer on return. Update the stale comment at `EditorApp.cpp:2045-2046` ("SubmitSceneToBatcher's gizmo + camera rect") to say the cover's overlays come from its packet and it has no gizmo. From Task 6 this extract also runs a `GpuSceneSync` that no mesh pass uploads; Task 6 adds the invalidate that recovers from it. `vp.mesh` stays unset there, as today, and it still does not call `ArmGraphViewportFrame`, so `selectedIds` stays empty. Task 12 adds `EditorApp::CaptureGraphViewportPng(` to the host-gate table (spec 4.4: a frame builder joins the table in the change that makes it one).

- [ ] **Step 4: Run `[extract]` and the client-runtime case.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
.\ArcaneTests.exe "ClientRuntime owns a Runtime"
```

Expected: PASS. The sprite case reports 4 vertices and a call count of 1. The registered-id case reports one world-space span whose `material` is the registered id. The two `[gpu][extract]` cases read a green-dominant pixel at `(80, 48)` and leave `RenderErrorCount` unchanged. The client-runtime case reports the system absent on `ClientRuntime`. Record the seed. The same `[extract]` filter in Tasks 6 and 7 re-runs the registered-id case and the GPU witness: `editorView` keeps the sprite in the visible set, and Task 7's view-projection is `packet.spriteView`, filled from that `editorView`.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Programs/ArcaneEditor/src/App/EditorApp.cpp Source/Runtime/ArcaneCore/src/Arcane/Base/ExtractAccounting.hpp Source/Runtime/ArcaneCore/src/Arcane/Base/ExtractAccounting.cpp Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.hpp Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.hpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.cpp Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.hpp Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp Source/Programs/ArcaneEditor/src/App/EditorApp.hpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/ExtractFrameTest.cpp Tests/ArcaneTests/src/ClientRuntimeTest.cpp Tests/ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): extract copies the sprite batch into the packet" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 6: GPU scene and the visible set

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/GpuSceneSync.hpp:285-388`
- Modify: `Source/Client/ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp:110`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp` (the sprite copy from Task 5 stays; this task adds the visible set and the mesh frame in front of it)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp` (delete the `PrepareSceneForRender` call beside `:545`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` (delete `PrepareSceneForRender` at `:1698`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorApp.cpp` (`CaptureGraphViewportPng`: invalidate the GPU-scene mirror after the cover renders)
- Modify every remaining `BuildGpuSceneFrame` call: `Tests/ArcaneTests/src/GpuSceneSyncTest.cpp:499,536,561,617,667,668`, `GpuSceneCullTest.cpp:417,524,570,606`, `MeshCullNodeTest.cpp:67,110,116`, `NriGraphPixelTest.cpp:2222,2302,2359,2388` (re-grepped: these are every `BuildGpuSceneFrame(` call under `Tests/` and `Source/` besides `GpuSceneHost.hpp:110` and the definition, `GpuSceneSync.hpp:285`), and both calls inside `Tests/ArcaneTests/src/OverlayWitnessTest.cpp` (`CheckOverlayClasses` and `CheckPerspectiveGridDepth`, which Task 3 wrote against `const MeshTable*`)
- Test: `Tests/ArcaneTests/src/ExtractFrameTest.cpp` (the `World` fixture Task 5 added)

**Interfaces:**
- Consumes: `BuildGpuSceneFrame` (`GpuSceneSync.hpp:285`), `GpuSceneSync` (`:92`), `GpuSceneSyncedGeneration` (`GpuScene.hpp:352`, `ARC_RENDER_API`, null returns 0), `BuildVisibleSet` (`VisibilitySystem.hpp:119`), `MeshTable::meshes` (`SceneResources.hpp:262`), `MeshEntry::data.sections` (`MeshBuilder.hpp:77`), `MeshSection::indexOffset` / `indexCount` (`:64-65`). `PrepareSceneForRender` (`GpuSceneHost.hpp:87-111`) is what the hosts stop calling. `CheckPerspectiveGridDepth`'s helpers (`FillCamera`, `GpuSceneWorld`, `MakeVehicle`, and the pixel readers) are the copies Task 3 pasted into `OverlayWitnessTest.cpp`. This task does not move them.
- Produces:

```cpp
struct MeshSectionRange
{
    Guid mesh{};
    std::uint32_t section = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
};

std::vector<MeshSectionRange> MeshSectionRanges(const MeshTable* table);

void BuildGpuSceneFrame(const GpuSceneMirror& m, const VisibleSet* vis,
                        std::span<const MeshSectionRange> sections,
                        const ViewTransform& view, GpuSceneFrame& out);
```

The `const MeshTable*` overload is deleted. `MeshSectionRange` and `MeshSectionRanges` live in `GpuSceneSync.hpp` and are `inline` like every function in that header (`GpuSceneSync` `:92`, `BuildGpuSceneFrame` `:285`), so they carry no export macro.

- [ ] **Step 1: Write the failing test.**

Append to `ExtractFrameTest.cpp`:

```cpp
TEST_CASE("extract publishes one mesh row and ignores a later transform edit", "[nri][extract][gpuscene]")
{
    World world;
    const Arcane::Guid mesh = world.Mesh(1);
    Astra::Entity spawned = world.Spawn({ 0.0f, 0.0f, 0.0f }, mesh);
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::BoundsSystem{}(world.reg);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.mesh.stats.total >= 1u);
    const std::uint32_t total = packet.mesh.stats.total;
    world.reg.GetComponent<Arcane::Transform>(spawned)->position = { 40.0f, 0.0f, 0.0f };
    CHECK(packet.mesh.stats.total == total);
    CHECK(packet.visible.entries.size() >= 1u);
}
```

Add these two methods inside `World` (Task 7 reuses them). `CameraProjection::Orthographic` is 0 and `Perspective` is 1 (`Components.hpp:256-260`); the orthographic camera sits on `root` at the origin, so `ActiveSceneCamera` frames `(0, 0)` with half-height 2:

```cpp
void AddOrthoCamera()
{
    Arcane::Camera cam;
    cam.active = true;
    cam.projection = Arcane::CameraProjection::Orthographic;
    cam.orthographicSize = 2.0f;
    reg.AddComponent<Arcane::Camera>(root, cam);
}
void AddPerspectiveCamera()
{
    Astra::Entity e = reg.CreateEntity();
    Arcane::Transform t;
    t.position = { 0.0f, 0.0f, 5.0f };
    reg.AddComponent<Arcane::Transform>(e, t);
    reg.SetParent(e, root);
    Arcane::Camera cam;
    cam.active = true;
    cam.projection = Arcane::CameraProjection::Perspective;
    cam.fovYDegrees = 50.0f;
    cam.nearZ = 0.05f;
    cam.farZ = 500.0f;
    reg.AddComponent<Arcane::Camera>(e, cam);
}
```

Today the mesh pass culls against the perspective camera when the scene has both (`PrepareSceneForRender`, `GpuSceneHost.hpp:91-110`: `views[1]` when `!SameView(meshView, mainView)`), and `f3-cull-blend`, `runtime-scene`, and `W5`'s counts depend on it. These two cases pin it: a mesh inside the perspective frustum but outside the orthographic view is counted, and a mesh inside the orthographic slab but past the perspective far plane (500 m) is not.

```cpp
TEST_CASE("meshes cull against the perspective mesh view, not the orthographic sprite view", "[nri][extract][gpuscene]")
{
    auto coarse = [](glm::vec3 position)
    {
        World world;
        world.AddOrthoCamera();
        world.AddPerspectiveCamera();
        world.Spawn(position, world.Mesh(1));
        Arcane::TransformPropagationSystem{}(world.reg);
        Arcane::BoundsSystem{}(world.reg);
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 200u, 100u };
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        REQUIRE(packet.mesh.stats.total == 1u);
        return packet.mesh.stats.coarseVisible;
    };
    // x = 30 at depth 55: inside the perspective frustum (half-width about 51 m),
    // outside the orthographic view (half-width 4 m).
    CHECK(coarse({ 30.0f, 0.0f, -50.0f }) == 1u);
    // z = -600: inside the orthographic slab (+-render.ortho2D.depthRange, 1000 m),
    // 605 m from the perspective eye, past its far plane.
    CHECK(coarse({ 0.0f, 0.0f, -600.0f }) == 0u);
}
```

The cover's invalidate is pinned by a source-order case (the cover has no device-less entry point; with a null device every sync is already a full rebuild, so a device-less behaviour test could not tell). `ExtractFrameTest.cpp` adds `"Helpers/ConstantScan.hpp"`, `<fstream>`, and `<sstream>`:

```cpp
TEST_CASE("the scene-save cover invalidates the GPU-scene mirror after its extract", "[nri][extract][gpuscene]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorApp.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t fn = text.find("bool EditorApp::CaptureGraphViewportPng(");
    REQUIRE(fn != std::string::npos);
    const std::size_t end = text.find("\n    }\n", fn);   // four-space-indented member definitions
    REQUIRE(end != std::string::npos);
    const std::string body = text.substr(fn, end - fn);
    const std::size_t extract = body.find("ExtractFrame(");
    const std::size_t render = body.find("RenderFrameOffscreen(");
    const std::size_t invalidate = body.find("GpuSceneInvalidate(m_runtime->Registry())");
    REQUIRE(extract != std::string::npos);
    REQUIRE(render != std::string::npos);
    REQUIRE(invalidate != std::string::npos);
    CHECK(extract < render);
    CHECK(render < invalidate);
    // Before the early return on a non-Presented outcome, so every outcome invalidates.
    const std::size_t early = body.find("return false;", render);
    CHECK((early == std::string::npos || invalidate < early));
}
```

- [ ] **Step 2: Run `[extract]`.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract publishes one mesh row"
.\ArcaneTests.exe "meshes cull against the perspective mesh view"
.\ArcaneTests.exe "the scene-save cover invalidates"
```

Expected: FAIL. `packet.mesh.stats.total` is 0. The sprite case from Task 5 still passes.

- [ ] **Step 3: Publish the visible set and the section span.**

`MeshSectionRanges`:

```cpp
std::vector<MeshSectionRange> MeshSectionRanges(const MeshTable* table)
{
    std::vector<MeshSectionRange> out;
    if (!table || !table->meshes)
        return out;
    for (const auto& [guid, entry] : *table->meshes)
    {
        std::uint32_t section = 0;
        for (const MeshSection& s : entry.data.sections)
        {
            out.push_back(MeshSectionRange{ guid, section, s.indexOffset, s.indexCount });
            ++section;
        }
    }
    return out;
}
```

Inside `BuildGpuSceneFrame`, both lookup sites (`GpuSceneSync.hpp:321-323` and `:360-363`) become:

```cpp
const MeshSectionRange* section = nullptr;
for (const MeshSectionRange& candidate : sections)
    if (candidate.mesh == key.mesh && candidate.section == key.section)
        section = &candidate;
if (!section)
    continue;
```

`section->indexOffset` and `section->indexCount` replace `section.indexOffset` and `section.indexCount` from the old `MeshSection`. The opaque/masked emit sort (`:349-357`) and the transparent `renderOrder`-then-depth sort (`:380-388`) stay.

Every old call `BuildGpuSceneFrame(mirror, vis, tablePtr, view, out)` becomes `BuildGpuSceneFrame(mirror, vis, MeshSectionRanges(tablePtr), view, out)`, including a null `tablePtr` (`MeshSectionRanges(nullptr)` returns an empty vector) and both calls in `OverlayWitnessTest.cpp`. `PrepareSceneForRender`'s call at `GpuSceneHost.hpp:110` uses the same wrapper. Hosts stop calling `PrepareSceneForRender`. The function stays for the tests that already call it.

`ExtractFrame`, before the sprite `RenderSubmissionSystem` call, so `MainVisibleSet` sees this frame's set. It is `PrepareSceneForRender` (`GpuSceneHost.hpp:87-111`) moved into extract, the two-view cull included: the main view culls sprites as `views[0]`, and the mesh view culls meshes as `views[1]` whenever it differs. `SameView` is `GpuSceneHost.hpp:71-74`; that header is ArcaneClient's, so `ExtractFrame.cpp` keeps a copy of the three-line comparison in its anonymous namespace.

```cpp
// The two views the hosts pass PrepareSceneForRender today: the main (sprite)
// view and the mesh view. Task 7 publishes both on the packet.
std::optional<ViewTransform> mainView;
std::optional<ViewTransform> meshView;
if (req.editorView)
{
    mainView = *req.editorView;
    meshView = *req.editorView;   // Edit: one camera for both (EditorAppFrame.cpp:1694-1697)
}
else
{
    if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
        mainView = cam->view;
    else if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
        mainView = rc->view;   // no scene camera: the host's stored view (a plugin's SetView)
    meshView = ActivePerspectiveSceneCamera(reg, req.viewport);
}
ViewTransform cullView = mainView.value_or(ViewTransform{});
cullView.viewport = req.viewport;

SceneVisibility* sv = reg.GetResource<SceneVisibility>();
if (!sv)
    sv = reg.EmplaceResource<SceneVisibility>();
const bool twoViews = meshView && !SameView(*meshView, cullView);
sv->views.resize(twoViews ? 2 : 1);
BuildVisibleSet(reg, cullView, sv->views[0]);
if (twoViews)
    BuildVisibleSet(reg, *meshView, sv->views[1]);
packet.visible = sv->views[0];
if (twoViews)
    packet.meshVisible = sv->views[1];

GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
if (!mirror)
    mirror = reg.EmplaceResource<GpuSceneMirror>();
GpuSceneStage stage;
GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(req.device), stage);
if (meshView)
{
    const std::vector<MeshSectionRange> sections = MeshSectionRanges(reg.GetResource<MeshTable>());
    BuildGpuSceneFrame(*mirror, twoViews ? &packet.meshVisible : &packet.visible, sections, *meshView, packet.mesh);
}
else
{
    // PrepareSceneForRender's no-mesh-view arm (GpuSceneHost.hpp:103-108): the
    // stage still lands; only the batch table is empty.
    packet.mesh.batches.clear();
    packet.mesh.args.clear();
    packet.mesh.visibleIndices.clear();
    packet.mesh.rowCount = stage.rowCapacity;
    packet.mesh.stats = {};
}
packet.mesh.stage = stage;
```

`GpuSceneSync` advances the mirror tick and consumes dirty state (`GpuSceneSync.hpp:209-212`, `:262-280`). The host call is gone, so each viewport frame syncs once, inside its extract. The scene-save cover's extract (Task 5) is a second sync on a frame whose `vp.mesh` stays unset, so no `GpuSceneSyncNode` uploads that stage, and the mirror's `lastModel` history has already moved past the rows it staged: exactly the hazard `EditorAppFrame.cpp:1686-1692` names. `CaptureGraphViewportPng` therefore stores the outcome of `RenderFrameOffscreen(vp)` and calls `Arcane::GpuSceneInvalidate(m_runtime->Registry())` unconditionally, before today's `!= Presented -> return false`. That is the R-F recovery the viewport path already uses (`EditorAppFrame.cpp:1766-1773`, `GpuSceneHost.hpp:126`): the next viewport sync is a full rebuild. `EditorApp.cpp` already includes `GpuSceneHost.hpp` (`:43`), and `GpuSceneInvalidate` is not a host-gate token. `RuntimeFrame.cpp` deletes the `PrepareSceneForRender` call at `:545`. The mesh-scene block at `:674-688` moves to immediately after `ExtractFrame` and before the `:753` branch. `MeshSceneDesc::scene` is `const GpuSceneFrame*` (`MeshNode.hpp:332`), so that block sets `meshScene.scene = &packet.mesh`. `EditorAppFrame.cpp:1698` deletes its `PrepareSceneForRender` call, and `:2088` becomes `m_meshScene.scene = &packet.mesh`. `GpuSceneInvalidate` at `RuntimeFrame.cpp:761` and `EditorAppFrame.cpp:1773` stays.

- [ ] **Step 4: Run the mesh tests.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
.\ArcaneTests.exe "[gpuscene]"
.\ArcaneTests.exe "[overlay]"
```

Expected: PASS. `[overlay]` covers `CheckOverlayClasses` and `CheckPerspectiveGridDepth`, both now calling the span overload. `[gpuscene]` covers the updated call sites in `GpuSceneSyncTest.cpp` and `GpuSceneCullTest.cpp`.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Programs/ArcaneEditor/src/App/EditorApp.cpp Source/Client/ArcaneRender/src/Arcane/Render/GpuSceneSync.hpp Source/Client/ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/ExtractFrameTest.cpp Tests/ArcaneTests/src/GpuSceneSyncTest.cpp Tests/ArcaneTests/src/GpuSceneCullTest.cpp Tests/ArcaneTests/src/MeshCullNodeTest.cpp Tests/ArcaneTests/src/NriGraphPixelTest.cpp Tests/ArcaneTests/src/OverlayWitnessTest.cpp
git commit -m "feat(render): extract publishes the visible set and the GPU scene" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 7: Cameras and views

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp` (fill `spriteView` and `meshView`; the cull view and the batch view-projection read them)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp:862-904` (`PushSceneCamera`, both warnings move to extract)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.hpp:344-345` (delete `m_warnedNoSceneCamera` and `m_warnedMultiSceneCamera`)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp:523-550` (the camera sweeps leave; the one `ExtractFrame` from Task 5 stays above `:753`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` `RenderSceneToViewport` (`:1697`, stop calling `ActivePerspectiveSceneCamera` for the world view)
- Test: `Tests/ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `ActiveSceneCamera` (`SceneCamera.hpp:57`) returns `std::optional<SceneCameraView>`. `ActivePerspectiveSceneCamera` (`:206`) returns `std::optional<ViewTransform>`. `using PerspectiveCameraView = ViewTransform` (`:146`). `SceneCameraView` fields are `worldCenter`, `halfHeight`, `view` (`SceneCameraView.hpp`). `ViewTransform` has `view`, `projection`, `viewport` and no `operator==` (`ViewTransform.hpp:50`). `BuildVisibleSet` takes a `ViewTransform`.
- Produces: `packet.spriteView` as `std::optional<SceneCameraView>`, `packet.meshView` as `std::optional<ViewTransform>`.

- [ ] **Step 1: Write the failing test.**

`ViewTransform` comparisons are field-wise. `AddOrthoCamera` and `AddPerspectiveCamera` are the `World` methods Task 6 added. Append:

```cpp
static void CheckView(const Arcane::ViewTransform& a, const Arcane::ViewTransform& b)
{
    CHECK(a.view == b.view);
    CHECK(a.projection == b.projection);
    CHECK(a.viewport == b.viewport);
}

TEST_CASE("runtime extract keeps the ortho sprite view and the perspective mesh view", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 200u, 100u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    const std::optional<Arcane::SceneCameraView> ortho = Arcane::ActiveSceneCamera(world.reg, req.viewport);
    const std::optional<Arcane::ViewTransform> persp = Arcane::ActivePerspectiveSceneCamera(world.reg, req.viewport);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    REQUIRE(ortho.has_value());
    REQUIRE(persp.has_value());
    CheckView(packet.spriteView->view, ortho->view);
    CHECK(packet.spriteView->worldCenter == ortho->worldCenter);
    CHECK(packet.spriteView->halfHeight == ortho->halfHeight);
    CheckView(*packet.meshView, *persp);
    CHECK(packet.spriteViewFromScene);
}

TEST_CASE("with no scene camera the sprite view is the host's stored view, not identity", "[nri][extract]")
{
    World world;   // no Camera component
    Arcane::RenderContext2D ctx;
    ctx.view = Arcane::Ortho2DView({ 3.0f, 1.0f }, 2.0f, { 64u, 64u });
    world.reg.SetResource<Arcane::RenderContext2D>(ctx);
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    CheckView(packet.spriteView->view, ctx.view);
    CHECK_FALSE(packet.spriteViewFromScene);
}

TEST_CASE("an editorView is the world view and is not replaced by a scene camera", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddPerspectiveCamera();
    world.reg.GetComponent<Arcane::Transform>(world.root)->position = { 4.0f, 0.0f, 0.0f };
    const std::optional<Arcane::SceneCameraView> scene = Arcane::ActiveSceneCamera(world.reg, { 64u, 64u });
    REQUIRE(scene.has_value());
    Arcane::ViewTransform editor = scene->view;
    editor.view[3][0] += 3.0f;
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 64u, 64u };
    req.editorView = editor;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    REQUIRE(packet.meshView.has_value());
    CHECK(packet.spriteView->view.view == editor.view);
    CHECK(packet.spriteView->view.projection == editor.projection);
    CHECK(packet.meshView->view == editor.view);
    CHECK(packet.meshView->projection == editor.projection);
    CHECK(packet.spriteView->worldCenter == glm::vec2(0.0f));
    CHECK(scene->worldCenter != packet.spriteView->worldCenter);
}
```

`AddOrthoCamera` and `AddPerspectiveCamera` are the `World` methods from Task 6.

- [ ] **Step 2: Run the two cases.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "runtime extract keeps the ortho sprite view"
.\ArcaneTests.exe "an editorView is the world view"
.\ArcaneTests.exe "with no scene camera the sprite view is the host's stored view"
```

Expected: FAIL. `spriteView` is empty.

- [ ] **Step 3: Fill the two views inside extract, and stop the host sweeps.**

Replace the `mainView` / `meshView` resolution at the top of the Task 6 cull block with this, which publishes both views; the rest of that block reads `mainView` and `meshView` exactly as before:

```cpp
if (req.editorView)
{
    packet.spriteView = SceneCameraView{};
    packet.spriteView->view = *req.editorView;
    packet.meshView = *req.editorView;
}
else
{
    int camCount = 0;
    packet.spriteView = ActiveSceneCamera(reg, req.viewport, &camCount);
    packet.spriteViewFromScene = packet.spriteView.has_value();
    packet.meshView = ActivePerspectiveSceneCamera(reg, req.viewport);
    if (!packet.spriteView && !warnedNoSceneCamera)
    {
        warnedNoSceneCamera = true;
        int total = 0;
        reg.CreateView<const Camera>().ForEach([&](Astra::Entity, const Camera&) { ++total; });
        if (total == 0)
            ARC_WARN("scene has no Camera component at all -- nothing sets the view. "
                     "Add a Camera component to an entity (a New Scene ships one).");
        else
            ARC_WARN("scene carries {} Camera component(s) but none is usable "
                     "as the sprite camera.", total);
    }
    if (camCount > 1 && !warnedMultiSceneCamera)
    {
        warnedMultiSceneCamera = true;
        ARC_WARN("scene carries {} active Camera entities -- the first found wins", camCount);
    }
    // No scene camera: keep the host's stored view, as PushSceneCamera does
    // today (RuntimeApp.cpp:864-870) -- a plugin driving ClientRuntime::SetView
    // still works. RenderContext2D::view is that stored view (ClientRuntime.cpp:125-133).
    if (!packet.spriteView)
        if (const RenderContext2D* rc = reg.GetResource<RenderContext2D>())
        {
            packet.spriteView = SceneCameraView{};
            packet.spriteView->view = rc->view;
        }
}
const std::optional<ViewTransform> mainView =
    packet.spriteView ? std::optional<ViewTransform>(packet.spriteView->view) : std::nullopt;
const std::optional<ViewTransform>& meshView = packet.meshView;
```

`warnedNoSceneCamera` and `warnedMultiSceneCamera` are function-local `static bool`s, the same two once-per-process warnings `PushSceneCamera` prints today (`RuntimeApp.cpp:878-898` and `:900-904`). `ActiveSceneCamera`'s third parameter, `int* outCount` (`SceneCamera.hpp:57-59`), is the active-camera count `PushSceneCamera` reads today. The `CreateView` lives in `ExtractFrame.cpp`. After this task `PushSceneCamera` warns about nothing, and the two `m_warned...` members (`RuntimeApp.hpp:344-345`) are deleted.

The cull view is still `mainView` with `viewport` set to `req.viewport`, and the two-view cull from Task 6 is unchanged. `BuildVisibleSet` receives that `ViewTransform`, not the `SceneCameraView`. The host batcher's `SetViewProjection` becomes `packet.spriteView ? packet.spriteView->view.ViewProjection() : glm::mat4{ 1.0f }` (identity only when there is neither a scene camera nor a `RenderContext2D`). `BuildGpuSceneFrame` receives `packet.meshView` when it has a value, and skips the batch build when it does not (Task 6's no-mesh-view arm).

`PushSceneCamera` (`RuntimeApp.cpp:862`) takes the packet and, when `packet.spriteViewFromScene` is true, calls `SetView(packet.spriteView->view)`; with no scene camera it leaves the stored view untouched, as today. It does not call `ActiveSceneCamera`. Move its call from `:525` to immediately after the `ExtractFrame` call that Task 5 placed above `:753`, and pass that packet. The order after `ExtractFrame` in `RenderGraph` is fixed: `PushSceneCamera(packet)`, then the mesh-scene block Task 6 moved, then the two pick blocks Task 8 moves (their `graphFrame.pickView = io.runtime->View()`, `RuntimeFrame.cpp:587`, `:629`, must read the view `PushSceneCamera` just installed), then the `RenderFrame` / `RenderFrameOffscreen` branch. `RuntimeFrame.cpp:543-546` deletes the `ActivePerspectiveSceneCamera` local. The one `ExtractFrame(` stays above both render paths. The editor's `RenderSceneToViewport` keeps passing `req.editorView` on the Edit path and leaves it empty when `InPlayMode()`. Delete the `ActivePerspectiveSceneCamera` call at `:1697`. After `ExtractFrame`, `m_meshView = packet.meshView`.

- [ ] **Step 4: Run the camera cases and the sprite case.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[extract]"
```

Expected: PASS. The editor case keeps `worldCenter` at 0 while the scene camera's `worldCenter` differs. The runtime case matches both live cameras field by field.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/ExtractFrameTest.cpp
git commit -m "feat(render): extract publishes the sprite and mesh views" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 8: Picking

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/PickEmit.cpp` (`CollectPickables` at `:32`; `BuildPickIdGeometry` stays at `:244`; `PickBoundHalfExtents` at `:227` stays because `:295` calls it)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp` (the moved function)
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp:572-589` and `:619-650` (both blocks move to after `ExtractFrame`; both `CollectPickables` calls leave; `io.pickDrawables` is copied from the packet; `--pick-probe` writes `selectedIds`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp:2175-2200` (`ArmGraphViewportFrame`'s `CollectPickables` leaves; the selection loop at `:2183-2193` writes `packet.selectedIds` and stops assigning `vp.selectedIds`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:1371-1372`
- Modify: `Tests/ArcaneTests/src/NriGraphPixelTest.cpp` (append the polarity case after `CheckSelectionOutlineTracesOnlyTheSelection`)
- Test: `Tests/ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `CollectPickables(Astra::Registry&, std::vector<PickDrawable>&)` (`PickEmit.cpp:32`). `PickEmit.hpp:24` includes `Astra/Entity/Entity.hpp` and `:36` forward-declares `Astra::Registry`. `PickPassId` (`:189`) uses `Astra::Entity` from that header. `PickSampleTexel` (`:199`) uses `std::floor` and `std::clamp`. `PickKindCode` (`:214`), `PickBoundHalfExtents` (`:227`), and `BuildPickIdGeometry` (`:244`) stay in this file; `BuildPickIdGeometry` reads `PickDrawable` and `glm` only. Today's includes, every one of which leaves: `SpriteGeometry.hpp` (`:4`), `VisibilitySystem.hpp` (`:5`), `Components.hpp` (`:6`), `PhysicsComponents.hpp` (`:7`, now an ArcanePhysics2D header at the same include path, which includes `Ecs.hpp` at `PhysicsComponents.hpp:7`), `PhysicsSystem.hpp` (`:8`, ArcanePhysics2D), `SceneResources.hpp` (`:9`, ArcaneScene, which includes `Ecs.hpp` at `SceneResources.hpp:7`, which includes `Registry.hpp` at `Ecs.hpp:24`), `Registry.hpp` (`:11`), and `<utility>` (`:17`). `PrepareSelection` takes `std::span<const std::uint32_t>` (`PickOutlineNodes.hpp:466`). `CurrentSelectedIds` is `NriGraphContext.hpp:1070`. `RenderFrame` and `RenderFrameOffscreen` null the bound packet before they return (Task 5).
- Produces: `packet.pickDrawables`. The selection loop and the `--pick-probe` stand-in write `packet.selectedIds` through `PacketRing::Latest()`. The outline reads that field when a packet is bound. After this task `PickEmit.cpp` includes only `PickEmit.hpp`, `Constant.hpp`, `glm/glm.hpp`, `<algorithm>`, and `<cmath>`.

- [ ] **Step 1: Write the failing test.**

```cpp
TEST_CASE("extract publishes a pick drawable that survives a later edit", "[nri][extract]")
{
    World world;
    Astra::Entity sprite = world.AddSprite({ 1.0f, 2.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);
    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 4.0f, req.viewport);
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.pickDrawables.size() == 1u);
    CHECK(packet.pickDrawables[0].entity == sprite);
    const glm::vec3 corner = packet.pickDrawables[0].corners[0];
    world.reg.GetComponent<Arcane::Transform>(sprite)->position = { 30.0f, 30.0f, 0.0f };
    CHECK(packet.pickDrawables[0].corners[0] == corner);
}
```

- [ ] **Step 2: Run the case.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract publishes a pick drawable"
```

Expected: FAIL. `pickDrawables` is empty.

- [ ] **Step 3: Move the collector, drop every registry include, and publish `selectedIds`.**

Move `RotateVec` (`PickEmit.cpp:24-29`) into `ExtractFrame.cpp`'s anonymous namespace, and `CollectPickables` (`:32` through the line before `PickBoundHalfExtents`) into `ExtractFrame.cpp` in `namespace Arcane`, **not** the anonymous namespace: its `ARC_RENDER_API` declaration at `PickEmit.hpp:107` stays, and `PickBufferTest.cpp` calls it directly (`:232`, `:288`, `:330`, `:354`, `:410`, `:421`; all `[pick]`, which Step 4 runs). Extract calls it so `packet.pickDrawables` receives the appended drawables. Spec 4.4 bans the hosts calling it, not the symbol. `ExtractFrame.cpp` includes `PickEmit.hpp`, `SpriteGeometry.hpp`, `Components.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp`, `SceneResources.hpp`, and `VisibilitySystem.hpp`. Those headers stay legal in this one TU.

`PickEmit.cpp` keeps `PickPassId`, `PickSampleTexel`, `PickKindCode`, `PickBoundHalfExtents`, and `BuildPickIdGeometry`. Delete each of these includes: `SpriteGeometry.hpp`, `VisibilitySystem.hpp`, `Components.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp`, `SceneResources.hpp`, `Registry.hpp`, and `<utility>`. The include list becomes:

```cpp
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Core/Constant.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
```

`packet.pickDrawables` is the list the pick node already consumes. The frame still passes `std::span<const PickDrawable>(packet.pickDrawables)` as `FrameDesc::pickables`. `FrameDesc::selectedIds` is not that list.

The Task 5 `ExtractFrame` call in `RenderSceneToViewport` stays after `SubmitSceneToBatcher` (`EditorAppFrame.cpp:1703`) and before `ArmGraphViewportFrame` (`:1726`), so the loop below reads this frame's drawables. `ArmGraphViewportFrame` deletes the clear and the `CollectPickables` call at `:2174-2175`. The selection loop at `:2183-2193` stays, still calls `PickPassIdOf` (the host-gate strip of that name stays), and writes the ids it already pushes:

`PickPassIdOf` takes `std::span<const PickDrawable>` (`PickEmit.hpp:151`). The loop reads the packet vector. `Latest()` is non-const; `ExtractFrame` has already returned. The click table at `:2217-2219` still walks `m_pickDrawables`, and `TakeRequest` inverts the id through that table, so the member is a copy of the packet vector.

```cpp
Arcane::RenderFramePacket& packet = m_packetRing.Latest();
m_pickDrawables = packet.pickDrawables;
m_pickSelectedIds.clear();
if (wantOutline)
{
    m_pickSelectedIds.reserve(m_selection.Count());
    for (Astra::Entity e : m_selection.Entities())
    {
        const std::uint32_t id = Arcane::PickPassIdOf(packet.pickDrawables, e);
        if (id != 0u)
            m_pickSelectedIds.push_back(id);
    }
}
packet.selectedIds = m_pickSelectedIds;
vp.pickables = packet.pickDrawables;
```

Replace `vp.pickables = m_pickDrawables` (`:2196`) with the snippet's `vp.pickables = packet.pickDrawables`. Delete `vp.selectedIds = m_pickSelectedIds` (`:2200`). `m_pickSelectedIds` may still be filled; it is not what the outline reads. `vp.pickOutline` and `vp.pickView` stay (`:2195`, `:2199`). The early return at `:2159-2166` still clears `m_pickDrawables` and `m_pickSelectedIds` and returns before the snippet, so `packet.selectedIds` stays empty because `BeginExtract` already cleared it. `wantOutline` false and `wantPick` true skips the loop, so `selectedIds` stays empty, which is what that frame does today. The `:2217-2219` walk is not rewritten: it reads the copy.

`AddOutlineNodes` (`PickOutlineNodes.cpp:1371-1372`) prefers the packet:

```cpp
if (OutlineNode* node = context ? context->Outline() : nullptr)
{
    if (context->Packet())
        node->PrepareSelection(context->Packet()->selectedIds);
    else
        node->PrepareSelection(context->CurrentSelectedIds());
}
```

When a packet is bound, `FrameDesc::selectedIds` is ignored. A frame that does not `BindPacket` keeps today's `CurrentSelectedIds()` path, which is what `CheckSelectionOutlineTracesOnlyTheSelection` (`NriGraphPixelTest.cpp:733`) and any `--pick-probe` run with no packet still use.

Runtime: delete both `CollectPickables` calls (`RuntimeFrame.cpp:575` and `:625`). The `--pick-probe` block (`:572-589`, including its `#if !defined(ARC_BUILD_DIST)` guard) and the `pick@` block (`:619-650`, which `main` wrapped in `#if defined(ARC_RUNTIME_HOST_VERIFY)` with its one-line comment at `:617-618` and `#endif` at `:652`; that guard and comment move with the block) sit before `graphFrame.post` (`:697`). Task 5's one `ExtractFrame` sits after that assignment and above the branch at `:753`, so both blocks run before this frame's packet exists. Move each block, comment text unchanged, to after that `ExtractFrame` call and before the `RenderFrame` / `RenderFrameOffscreen` branch, in the order Task 7 fixed: `ExtractFrame`, `PushSceneCamera(packet)`, the mesh-scene block, then these two blocks, so their `graphFrame.pickView = io.runtime->View()` (`:587`, `:629`) reads the view `PushSceneCamera` just installed. `packet` is then in scope and `Latest()` is this frame's slot. In place of each `CollectPickables` call, assign `io.pickDrawables = packet.pickDrawables`. `FrameIo::pickDrawables` is `RuntimeApp::m_pickDrawables` (`RuntimeApp.cpp:765`), and shutdown inverts the probe through `PickEntityForId(m_pickDrawables, *id)` at `:947` (inside `#if !defined(ARC_BUILD_DIST)`, `:916-954`) and `:1065` (inside `#if defined(ARC_RUNTIME_HOST_VERIFY)`, `:970-1099`). An empty member reports every hit as unmapped. Replace `graphFrame.pickables = io.pickDrawables` at `:586` and `:628` with `packet.pickDrawables`. `graphFrame.pickView` (`:587`, `:629`) and `graphFrame.pickOutline` stay. Delete `graphFrame.selectedIds = io.pickSelectedIds` (`:588`). `--pick-probe`, after the move and only when `packet.pickDrawables` is non-empty, writes `io.packetRing.Latest().selectedIds = { 1u }`. It may still fill `io.pickSelectedIds` (`:581-583`) so that member is not stale; the outline does not read it. The `pick@` block leaves `selectedIds` empty and keeps `hoverPixel = (-1, -1)`. The comment at `:631-648` stays true and is not edited, and neither are the `CollectPickables` comments at `:556` and `:579`.

Append to `NriGraphPixelTest.cpp` after `CheckSelectionOutlineTracesOnlyTheSelection`. `MakeVehicle`, `RenderOne`, `At`, `WorldQuadAtPixel` (`:534`), `PixelIdentityView` (`:529`), and `DifferingTexels` (`:713`) are already in this TU's anonymous namespaces. `BindPacket` is cleared before `RenderFrameOffscreen` returns, so each capture binds immediately before its `RenderOne`. `NodeSet::pickOutline = true`. Both backends, tagged `[gpu][pixel][pick]`.

```cpp
std::vector<unsigned char> CaptureBoundSelection(
    PixelVehicle& v, const Arcane::RenderFramePacket* packet,
    std::span<const Arcane::PickDrawable> drawables,
    std::span<const std::uint32_t> frameSelected,
    std::uint32_t& w, std::uint32_t& h)
{
    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture     = true;
    frame.pickOutline = true;
    frame.pickables   = drawables;
    frame.pickView    = PixelIdentityView();
    frame.selectedIds = frameSelected;
    frame.hoverPixel  = glm::ivec2(-1, -1);
    if (packet)
        v.ctx->BindPacket(packet);
    RenderOne(*v.ctx, frame);
    std::vector<unsigned char> rgba;
    REQUIRE(v.ctx->ReadCapture(w, h, rgba));
    return rgba;
}

void CheckPacketSelectionDrawsTheOutline(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();
    Arcane::NriGraphContext::NodeSet nodes;
    nodes.pickOutline = true;

    const Arcane::PickDrawable quad = WorldQuadAtPixel({ 60.0f, 40.0f }, { 20.0f, 14.0f });
    Arcane::RenderFramePacket fromPacket;
    fromPacket.pickDrawables.push_back(quad);
    fromPacket.selectedIds = { 1u };
    Arcane::RenderFramePacket fromFrameIgnored;
    fromFrameIgnored.pickDrawables.push_back(quad);

    std::uint32_t wP = 0, hP = 0, wF = 0, hF = 0, wB = 0, hB = 0;
    std::vector<unsigned char> packetPx, framePx, baseline;
    {
        PixelVehicle v = MakeVehicle(backend, nodes);
        packetPx = CaptureBoundSelection(v, &fromPacket, fromPacket.pickDrawables, {}, wP, hP);
    }
    {
        PixelVehicle v = MakeVehicle(backend, nodes);
        const std::uint32_t selected[] = { 1u };
        framePx = CaptureBoundSelection(v, &fromFrameIgnored, fromFrameIgnored.pickDrawables, selected, wF, hF);
    }
    {
        PixelVehicle v = MakeVehicle(backend, nodes);
        const Arcane::PickDrawable drawables[] = { quad };
        baseline = CaptureBoundSelection(v, nullptr, drawables, {}, wB, hB);
    }
    REQUIRE(wP == wB && hP == hB && wF == wB && hF == hB);
    CHECK(DifferingTexels(packetPx, baseline, wB, hB) > 40u);
    CHECK(DifferingTexels(framePx, baseline, wB, hB) == 0u);
    CHECK(Arcane::RenderErrorCount() == before);
}
```

`fromPacket` binds a packet whose `selectedIds` is `{1}` and whose `FrameDesc::selectedIds` is empty. `fromFrameIgnored` binds the same drawable with an empty `packet.selectedIds` and `FrameDesc::selectedIds` of `{1}`. `baseline` does not bind a packet and selects nothing. The first check fails if the outline still reads only `FrameDesc`. The second fails if a bound packet still honours `FrameDesc::selectedIds`. The floor of 40 is the same floor `CheckSelectionOutlineTracesOnlyTheSelection` uses. Add the two `TEST_CASE`s beside that function's pair, named `"packet selection draws the outline (d3d12)"` and `"packet selection draws the outline (vulkan)"`, tagged `[gpu][pixel][pick][nri]` plus the backend tag.

- [ ] **Step 4: Run the pick case.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract publishes a pick drawable"
.\ArcaneTests.exe "[pick]"
.\ArcaneTests.exe "packet selection draws the outline"
```

Expected: PASS. The polarity case fails if the outline still reads only `FrameDesc::selectedIds`.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Client/ArcaneRender/src/Arcane/Render/PickEmit.cpp Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp Tests/ArcaneTests/src/ExtractFrameTest.cpp Tests/ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): extract publishes pick drawables" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 9: Overlays

**Files:**
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Grid2D.hpp`
- Create: `Source/Client/ArcaneRender/src/Arcane/Render/Grid2D.cpp`
- Modify: `Source/Programs/ArcaneEditor/src/Viewport/ViewportGrid.hpp` and `ViewportGrid.cpp` (`PlanGrid2D` at `:78`, `DrawGrid2D` at `:132`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp`
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp` (`DeclareGraphFrame`, the `wantOverlay` block Task 3 added after `AddTonemapNode`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.cpp` (`Record` reads `Packet()->overlay` when a packet is bound)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` (`SubmitSceneToBatcher` `:1866-2001`, the `vp.grid` block at `:2118-2129`, the gizmo draw read at `:3730`, and the Task 3 `FrameDesc::overlay` pointer)
- Modify: `Tests/ArcaneTests/src/NriGraphPixelTest.cpp` (append the extract-to-present case)
- Test: `Tests/ArcaneTests/src/ExtractFrameTest.cpp`
- `Tests/ArcaneTests/src/ViewportGridTest.cpp` stays green without an edit

**Interfaces:**
- Consumes: `DrawGrid2D` (`ViewportGrid.cpp:132` through its closing brace), `PlanGrid2D` (`:78`), `PixelsPerMetre` (`:70`). `DrawPhysicsDebug` (`Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.hpp:110-113`, `ARC_RENDER_API`) is documented to call only `Line`, `Circle`, and `Rect` (`:88-91`); since the modularization split it is `GatherPhysicsDebug` (ArcanePhysics2D, `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.hpp`) followed by `DrawGeometry`, which emits only `Line` and `Circle` (`PhysicsDebugDraw.cpp:22-32`, `:34-39`). The collector below still defines `Rect`, which is pure virtual. `Batcher2D`'s pure virtuals, in declaration order: `Begin` (`:238`), `SetLayer` (`:251`), `Quad` (`:260`), `Glyph` (`:286`), `Rect` (`:292`), `Line` (`:294`), `Circle` (`:296`), `Triangle` (`:302`), `End` (`:322`), `Stats` (`:326`), `QuadWorld` (`:406`), `CircleWorld` (`:413`), `SetViewProjection` (`:422`). `RegisterMaterial`, `UpdateMaterial`, `SetGlobals`, `QuadMaterial`, `Drain`, `MaterialDesc`, and `QuadTextured` have defaults. `kCameraFrameColor` is `{ 0.45, 0.62, 0.78, 0.75 }` (`EditorThemeSettings.hpp:181`). `EditorGridSettings` fields read at `ViewportGrid.cpp:142-145` are `minorAlpha`, `majorAlpha`, `lineColor`, `lineThickness`, `maxLinesPerAxis`.
- Produces: `Arcane::PlanGrid2D(float ppm, float minorAlpha, float majorAlpha)`, `Arcane::EmitGrid2D(...)`, and the local collector below. `Editor::PlanGrid2D` and `Editor::DrawGrid2D` keep their signatures so `ViewportGridTest.cpp` stays green.

`DrawPhysicsDebug` submits pixels through `PhysicsDebugDrawOptions2D::view` (`EditorAppFrame.cpp:1920-1936`; `PhysicsDebugDrawOptions2D` is `PhysicsDebugGeometry.hpp:16`). The collector stores those calls as screen-space overlay primitives. Grid lines are the world endpoints `DrawGrid2D` already computes before `WorldToScreen` (`ViewportGrid.cpp:166-175`), stored with `screenSpace == false` so the overlay shader projects them.

- [ ] **Step 1: Write the failing test.**

```cpp
TEST_CASE("overlay tags: grid is depth-tested linear, physics camera and gizmo are always on top", "[nri][extract]")
{
    World world;
    world.AddOrthoCamera();
    world.AddSprite({ 0.0f, 0.0f, 0.0f });
    Arcane::TransformPropagationSystem{}(world.reg);

    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    // PhysicsWorld2D owns the solver privately; tests adopt one (PhysicsPauseTest.cpp:25).
    world.reg.SetResource(Arcane::Detail::Physics2D::Adopt(
        std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
    Manifold2D::Physics::PhysicsWorld& solver =
        *Arcane::Detail::Physics2D::Access::Solver(*world.reg.GetResource<Arcane::PhysicsWorld2D>());
    Manifold2D::Physics::BodyDef bd;
    bd.type = Manifold2D::Physics::BodyType::Static;
    bd.position = Manifold2D::Physics::Vec2(Manifold2D::Physics::Real(0), Manifold2D::Physics::Real(0));
    bd.shape = Manifold2D::Physics::MakeAabb(Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
    solver.AddBody(bd);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 128u, 128u };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 2.0f, req.viewport);
    req.showGrid2d = true;
    req.gridMinorAlpha = 0.35f;
    req.gridMajorAlpha = 0.55f;
    req.gridLineRgb = { 0.5f, 0.5f, 0.5f };
    req.gridLineThicknessPx = 1.0f;
    req.gridMaxLinesPerAxis = 4096;
    req.physicsDebug = true;
    req.physicsWholeWorld = true;
    req.cameraRect = true;
    Arcane::OverlayLine gizmo{};
    gizmo.a = { 0.0f, 0.0f, 0.0f };
    gizmo.b = { 10.0f, 0.0f, 0.0f };
    gizmo.color = { 1.0f, 0.0f, 0.0f, 1.0f };
    gizmo.thickness = 1.0f;
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.lines.push_back(gizmo);

    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    bool sawGrid = false;
    bool sawGizmo = false;
    bool sawScreenLinear = false;
    for (const Arcane::OverlayLine& line : packet.overlay.lines)
    {
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::DepthTested &&
            line.screenSpace == false)
            sawGrid = true;
        if (line.space == Arcane::OverlayColorSpace::DisplayReferred)
        {
            sawGizmo = true;
            CHECK(line.depth == Arcane::OverlayDepthClass::AlwaysOnTop);
            CHECK(line.screenSpace == true);
        }
        if (line.space == Arcane::OverlayColorSpace::Linear &&
            line.depth == Arcane::OverlayDepthClass::AlwaysOnTop &&
            line.screenSpace == true)
            sawScreenLinear = true;
    }
    CHECK(sawGrid);
    CHECK(sawGizmo);
    CHECK(sawScreenLinear);
}
```

A selected body outlines only that body (today's Edit-mode overlay with View -> Physics Overlay off). The two static boxes sit either side of the view's centre column, `x = 80` (`Ortho2DView({0,0}, 16, {160,96})` is 3 px/m with world `(0,0)` at screen `(80,48)`, the mapping the backbuffer case below derives), so every physics line is classified by its midpoint's side. `Access::Entities` is the entity-to-body map extract reads; the test writes it directly instead of running `PhysicsSystem2D`. Append to `ExtractFrameTest.cpp`, which already includes `<Arcane/Scene/PhysicsSystem.hpp>` and the Manifold2D headers for the case above; add `<optional>` and `<utility>`.

```cpp
TEST_CASE("a selected body outlines only that body", "[nri][extract]")
{
    World world;
    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    world.reg.SetResource(Arcane::Detail::Physics2D::Adopt(
        std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
    Arcane::PhysicsWorld2D& physics = *world.reg.GetResource<Arcane::PhysicsWorld2D>();
    Manifold2D::Physics::PhysicsWorld& solver = *Arcane::Detail::Physics2D::Access::Solver(physics);
    auto addBox = [&](float x)
    {
        Manifold2D::Physics::BodyDef bd;
        bd.type = Manifold2D::Physics::BodyType::Static;
        bd.position = Manifold2D::Physics::Vec2(Manifold2D::Physics::Real(x), Manifold2D::Physics::Real(0));
        bd.shape = Manifold2D::Physics::MakeAabb(Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
        const Astra::Entity e = world.reg.CreateEntity();
        Arcane::Detail::Physics2D::Access::Entities(physics)[e] = solver.AddBody(bd);
        return e;
    };
    const Astra::Entity left = addBox(-6.0f);
    addBox(6.0f);
    // The Play rows render through the scene camera: the same 3 px/m framing
    // (orthographicSize is the half-height, SceneCamera.hpp:86).
    world.AddOrthoCamera();
    world.reg.GetComponent<Arcane::Camera>(world.root)->orthographicSize = 16.0f;

    // Physics lines are the screen-space, linear, always-on-top lines: no grid,
    // no camera rect, and no gizmo are requested here.
    auto sides = [&](std::optional<Astra::Entity> selected, bool wholeWorld, bool play = false)
    {
        Arcane::PacketRing ring;
        Arcane::ExtractRequest req;
        req.viewport = { 160u, 96u };
        req.playMode = play;
        if (!play)
            req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 16.0f, req.viewport);
        req.physicsDebug = true;
        req.physicsWholeWorld = wholeWorld;
        req.physicsSelectedBody = selected;
        const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);
        int leftLines = 0;
        int rightLines = 0;
        for (const Arcane::OverlayLine& line : packet.overlay.lines)
        {
            if (!line.screenSpace || line.space != Arcane::OverlayColorSpace::Linear
                || line.depth != Arcane::OverlayDepthClass::AlwaysOnTop)
                continue;
            if (0.5f * (line.a.x + line.b.x) < 80.0f)
                ++leftLines;
            else
                ++rightLines;
        }
        return std::pair<int, int>{ leftLines, rightLines };
    };

    const auto [selectedLeft, selectedRight] = sides(left, false);
    CHECK(selectedLeft > 0);
    CHECK(selectedRight == 0);
    const auto [noneLeft, noneRight] = sides(std::nullopt, false);
    CHECK(noneLeft == 0);
    CHECK(noneRight == 0);
    const auto [allLeft, allRight] = sides(std::nullopt, true);
    CHECK(allLeft > 0);
    CHECK(allRight > 0);
    // Play: the toggled whole-world overlay still draws, with no editor camera;
    // a selection alone draws nothing (PlanPhysicsOverlay's Edit-only outline).
    const auto [playAllLeft, playAllRight] = sides(std::nullopt, true, true);
    CHECK(playAllLeft > 0);
    CHECK(playAllRight > 0);
    const auto [playSelLeft, playSelRight] = sides(left, false, true);
    CHECK(playSelLeft == 0);
    CHECK(playSelRight == 0);
}
```

Append to `NriGraphPixelTest.cpp`, after the helpers `MakeVehicle`, `RenderOne`, `At`, `Luma`, `kW`, and `kH` (`:140-235`). Add `#include <Arcane/Render/ExtractFrame.hpp>`, `#include <Arcane/Scene/PhysicsSystem.hpp>`, `#include <Arcane/Scene/SceneModule.hpp>`, `#include <Astra/Component/ComponentRegistry.hpp>`, and `<cmath>`. `PhysicsWorld2D` is `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/Physics2DWorld.hpp:67` (reached through `Physics2D.hpp:8`); `Detail::Physics2D::Access` and `Adopt` are `Physics2DDetail.hpp:33-54`, which `PhysicsSystem.hpp:9` includes. That header includes `Ecs.hpp`; this TU is under `Tests/ArcaneTests`, not `Source/Client/ArcaneRender/src/Arcane/Render`, so the include gate does not see it. Do not set `frame.overlay` or `frame.grid` on either capture.

`Ortho2DView({0,0}, 16.0f, {kW, kH})` (`RenderViewSettings.hpp:33`) gives `halfW = 16 * (160/96) = 80/3`. `AsAffine2D` (`ViewTransform.hpp:113-127`) then has `scale.x = 3` px/m and `scale.y = -3`, and `offset = (80, 48)`. `WorldToScreen` (`ViewTransform.hpp:67`) maps world `(0,0,0)` to screen `(80, 48)`. +Y is up and screen Y is down. The Y axis is the world line `x = 0` and the X axis is `y = 0` (`ViewportGrid.cpp:224-231`). With `gridLineThicknessPx = 8` the screen thickness is 8 px, so the Y axis covers x `76-84` and the X axis covers y `44-52`. Sample the Y axis at `(80, 20)`.

`MakeAabb` takes half-extents (`Shapes.hpp:156`). A static body at `(15, 6)` with `MakeAabb(1, 1)` is the world box x `14..16`, y `5..7`. `Affine2D::Point` (`ViewTransform.hpp:44`) puts that box near screen x `122-128`, y `27-33`, clear of both axis quads, of the gizmo, and of the 10 m and 20 m grid lines (screen x about `110` and `140`, 8 px thick: `106-114` and `136-144`), so a changed texel there is the physics line and not the grid. `cameraRect` is false, so every screen-space linear always-on-top line is physics, not the camera rect. `MakePhysicsDebugDrawOptions` (`Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.cpp:11-19`) copies the published `PhysicsDebugSettings2D`, whose `lineThickness` default is `1.0f` (`Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugTypes.hpp:7`). The test samples a neighbourhood because that line is one pixel wide.

```cpp
void CheckExtractedOverlayReachesTheBackbuffer(Arcane::GraphicsBackend backend)
{
    ARC_REQUIRE_BACKEND(backend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{ components };
    Arcane::RegisterSceneComponents(reg);

    Manifold2D::Physics::WorldDef wd;
    wd.gravityX = Manifold2D::Physics::Real(0);
    wd.gravityY = Manifold2D::Physics::Real(0);
    reg.SetResource(Arcane::Detail::Physics2D::Adopt(
        std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd)));
    Manifold2D::Physics::PhysicsWorld& solver =
        *Arcane::Detail::Physics2D::Access::Solver(*reg.GetResource<Arcane::PhysicsWorld2D>());
    Manifold2D::Physics::BodyDef bd;
    bd.type = Manifold2D::Physics::BodyType::Static;
    bd.position = Manifold2D::Physics::Vec2(
        Manifold2D::Physics::Real(15), Manifold2D::Physics::Real(6));
    bd.shape = Manifold2D::Physics::MakeAabb(
        Manifold2D::Physics::Real(1), Manifold2D::Physics::Real(1));
    solver.AddBody(bd);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { kW, kH };
    req.editorView = Arcane::Ortho2DView({ 0.0f, 0.0f }, 16.0f, req.viewport);
    req.playMode = false;
    req.showGrid2d = true;
    req.gridMinorAlpha = 0.35f;
    req.gridMajorAlpha = 0.55f;
    req.gridLineRgb = { 0.5f, 0.5f, 0.5f };
    req.gridLineThicknessPx = 8.0f;
    req.gridMaxLinesPerAxis = 4096;
    req.physicsDebug = true;
    req.physicsWholeWorld = true;
    req.cameraRect = false;
    Arcane::OverlayLine gizmo{};
    gizmo.a = { 4.0f, 10.0f, 0.0f };
    gizmo.b = { 16.0f, 10.0f, 0.0f };
    gizmo.color = { 0.0f, 0.0f, 1.0f, 1.0f };
    gizmo.thickness = 8.0f;
    gizmo.depth = Arcane::OverlayDepthClass::AlwaysOnTop;
    gizmo.space = Arcane::OverlayColorSpace::DisplayReferred;
    gizmo.screenSpace = true;
    req.gizmoPrims.lines.push_back(gizmo);

    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(reg, req, ring);
    REQUIRE(packet.spriteView.has_value());
    const glm::vec3 origin = packet.spriteView->view.WorldToScreen({ 0.0f, 0.0f, 0.0f });
    CHECK(std::abs(origin.x - 80.0f) < 0.5f);
    CHECK(std::abs(origin.y - 48.0f) < 0.5f);

    const Arcane::OverlayLine* physics = nullptr;
    float best = -1.0f;
    for (const Arcane::OverlayLine& line : packet.overlay.lines)
    {
        if (!line.screenSpace || line.space != Arcane::OverlayColorSpace::Linear
            || line.depth != Arcane::OverlayDepthClass::AlwaysOnTop)
            continue;
        const float len = glm::length(glm::vec2(line.b) - glm::vec2(line.a));
        if (len > best)
        {
            best = len;
            physics = &line;
        }
    }
    REQUIRE(physics != nullptr);
    const int px = (int)std::lround(0.5f * (physics->a.x + physics->b.x));
    const int py = (int)std::lround(0.5f * (physics->a.y + physics->b.y));
    REQUIRE(std::abs(px - 80) > 8 || std::abs(py - 20) > 8);
    REQUIRE(std::abs(px - 10) > 8 || std::abs(py - 10) > 8);

    Arcane::RenderFramePacket cleared = packet;
    cleared.overlay.lines.clear();
    cleared.overlay.fills.clear();
    cleared.overlay.hasGrid3d = false;

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;

    PixelVehicle v = MakeVehicle(backend);
    v.ctx->BindPacket(&packet);
    RenderOne(*v.ctx, frame);
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> drawn;
    REQUIRE(v.ctx->ReadCapture(w, h, drawn));

    v.ctx->BindPacket(&cleared);
    RenderOne(*v.ctx, frame);
    std::uint32_t w2 = 0, h2 = 0;
    std::vector<unsigned char> bare;
    REQUIRE(v.ctx->ReadCapture(w2, h2, bare));
    REQUIRE(w == kW && h == kH && w2 == w && h2 == h);

    const Rgba gridOn = At(drawn, w, 80, 20);
    const Rgba gridOff = At(bare, w, 80, 20);
    CHECK(gridOn.r != gridOff.r || gridOn.g != gridOff.g || gridOn.b != gridOff.b);

    const Rgba gizmoOn = At(drawn, w, 10, 10);
    CHECK(gizmoOn.r == 0);
    CHECK(gizmoOn.g == 0);
    CHECK(gizmoOn.b == 255);
    const Rgba gizmoOff = At(bare, w, 10, 10);
    CHECK(!(gizmoOff.r == 0 && gizmoOff.g == 0 && gizmoOff.b == 255));

    bool physicsDiff = false;
    const glm::ivec2 samples[5] = {
        { px, py }, { px - 1, py }, { px + 1, py }, { px, py - 1 }, { px, py + 1 } };
    for (const glm::ivec2 s : samples)
    {
        if (s.x < 0 || s.y < 0 || s.x >= (int)w || s.y >= (int)h)
            continue;
        const Rgba a = At(drawn, w, (std::uint32_t)s.x, (std::uint32_t)s.y);
        const Rgba b = At(bare, w, (std::uint32_t)s.x, (std::uint32_t)s.y);
        if (a.r != b.r || a.g != b.g || a.b != b.b)
            physicsDiff = true;
    }
    CHECK(physicsDiff);
    CHECK(Arcane::RenderErrorCount() == before);
}
```

The chebyshev test (`abs(dx) > 8 || abs(dy) > 8`) rejects a sample that is inside the 8 px square around `(80, 20)` or `(10, 10)` and accepts one that is outside it. The axis colour is `Grid2DStyle::axisY` (`{0.3, 0.8, 0.3, 0.9}`) after ACES and a source-over blend, so the grid check is "the texel changed", not an exact byte. The gizmo is display-referred, alpha 1, so `(10, 10)` is `(0, 0, 255)` and the cleared frame (clear `{0.02, 0.02, 0.04}` from `RenderDeviceSettings.hpp:83`, then ACES) is not. One `PixelVehicle`, two `RenderOne`s. `BindPacket` before each, because Task 5 nulls the pointer before return. Add `"extracted overlay reaches the backbuffer (d3d12)"` and `"extracted overlay reaches the backbuffer (vulkan)"`, tagged `[gpu][pixel][overlay][nri]` plus the backend tag.

- [ ] **Step 2: Run the cases.**

The GPU case is a new `TEST_CASE` in `NriGraphPixelTest.cpp`, so this step builds before it runs.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "overlay tags"
.\ArcaneTests.exe "a selected body outlines only that body"
.\ArcaneTests.exe "extracted overlay reaches the backbuffer"
```

Expected: FAIL. `packet.overlay.lines` is empty, and the backbuffer case finds no physics line or no overlay pixel. The pass still reads `FrameDesc`, and this case sets neither `frame.overlay` nor `frame.grid`.

- [ ] **Step 3: Move the grid, add the collector, and append the editor primitives.**

`Grid2D.hpp` (ArcaneRender; no editor settings, no `Registry.hpp`; includes `<Arcane/Modules/ArcaneRenderApi.hpp>`, `<Arcane/Render/OverlayPrimitive.hpp>`, `<Arcane/Scene/ViewTransform.hpp>`, `<glm/glm.hpp>`, `<cstdint>`, and `<vector>`):

```cpp
struct Grid2DStyle
{
    float minorAlpha = 0.35f;
    float majorAlpha = 0.55f;
    glm::vec3 lineRgb{ 0.5f, 0.5f, 0.5f };
    float lineThicknessPx = 1.0f;
    std::int64_t maxLinesPerAxis = 4096;
    glm::vec4 axisX{ 0.85f, 0.25f, 0.25f, 0.9f };
    glm::vec4 axisY{ 0.3f, 0.8f, 0.3f, 0.9f };
};

ARC_RENDER_API Grid2DPlan PlanGrid2D(float pixelsPerMetre, float minorAlpha, float majorAlpha) noexcept;

ARC_RENDER_API void EmitGrid2D(const ViewTransform& view, const Grid2DPlan& plan, const Grid2DStyle& style,
                               std::vector<OverlayLine>& out);
```

`GridLevel` and `Grid2DPlan` move with it from `ViewportGrid.hpp:85-97`, and `kMinDecade` / `kMaxDecade` (`ViewportGrid.cpp:24-27`) move into `Grid2D.cpp` with their `ARC_CONSTANT` markers; any other numeric `constexpr` this task adds needs one too, or `ConstantGuardTest.cpp`'s `[guard]` scan fails the suite gate. `Editor::PlanGrid2D(float)` reads `Settings<EditorGridSettings>()` and calls `Arcane::PlanGrid2D`. `EmitGrid2D` is the body of `DrawGrid2D` (`ViewportGrid.cpp:134` through its closing brace) with two substitutions. The `line` lambda pushes an `OverlayLine` instead of calling `b.Line`:

```cpp
OverlayLine line{};
line.a = glm::vec3(a, 0.0f);
line.b = glm::vec3(wb, 0.0f);
const float ppm = PixelsPerMetre(view);
line.thickness = ppm > 0.0f ? style.lineThicknessPx / ppm : 0.0f;
line.color = colour;
line.depth = OverlayDepthClass::DepthTested;
line.space = OverlayColorSpace::Linear;
line.screenSpace = false;
if (line.thickness > 0.0f)
    out.push_back(line);
```

Axis lines use `style.axisX` and `style.axisY` and the same tags. Delete `b.SetLayer`. `Editor::DrawGrid2D` calls `EmitGrid2D`, then for each stored line calls `b.SetLayer(0, 0)` once and `b.Line(glm::vec2(view.WorldToScreen(line.a)), glm::vec2(view.WorldToScreen(line.b)), line.thickness * PixelsPerMetre(view), line.color)` (`WorldToScreen` is the `ViewTransform` member, `ViewTransform.hpp:67`, and returns `glm::vec3`; `Batcher2D::Line` takes `glm::vec2`, the same conversion `ViewportGrid.cpp:169-172` makes, including its `Finite2` skip of a non-finite endpoint). That keeps `ViewportGridTest.cpp` on screen pixels.

`PixelsPerMetre` moves to `Arcane::` as well, declared `[[nodiscard]] ARC_RENDER_API float PixelsPerMetre(const ViewTransform& view) noexcept;` in `Grid2D.hpp` (today `Editor::PixelsPerMetre`, `ViewportGrid.hpp:102`). `Editor::PixelsPerMetre` forwards to it. The editor keeps `Grid2DAxisColors` (`ViewportGrid.hpp:79`) and its `DrawGrid2D` signature (`:112`); ArcaneEditor is a program and may call into ArcaneRender, and nothing in ArcaneRender reaches back into the editor.

The collector is `OverlayBatchCollector` from Task 3 (`Source/Client/ArcaneRender/src/Arcane/Render/OverlayBatchCollector.hpp`); `ExtractFrame.cpp` includes that header. Constructed with only the list, it tags every primitive `AlwaysOnTop`, `Linear`, `screenSpace = true`, which is what physics debug needs here.


Extract, after the views exist:

```cpp
if (req.showGrid2d && packet.spriteView)
{
    const ViewTransform& view = packet.spriteView->view;
    const float ppm = PixelsPerMetre(view);
    Grid2DStyle style;
    style.minorAlpha = req.gridMinorAlpha;
    style.majorAlpha = req.gridMajorAlpha;
    style.lineRgb = req.gridLineRgb;
    style.lineThicknessPx = req.gridLineThicknessPx;
    style.maxLinesPerAxis = req.gridMaxLinesPerAxis;
    style.axisX = req.gridAxisX;
    style.axisY = req.gridAxisY;
    EmitGrid2D(view, PlanGrid2D(ppm, style.minorAlpha, style.majorAlpha), style, packet.overlay.lines);
}
if (req.showGrid3d)
{
    packet.overlay.hasGrid3d = true;
    packet.overlay.grid3d = req.grid3d;
}
// Edit and Play alike (today's overlay draws through RenderContext2D::view in
// both, EditorAppFrame.cpp:1920-1936): the affine is the view this frame
// renders -- the editor camera in Edit, the scene (or host-stored) camera in Play.
if (req.physicsDebug && packet.spriteView)
{
    if (const PhysicsWorld2D* phys = reg.GetResource<PhysicsWorld2D>())
    {
        // The selected body, found the way EditorAppFrame.cpp:1915-1917 finds it today.
        std::optional<Detail::Physics2D::BodyHandle> selectedBody;
        if (req.physicsSelectedBody)
            if (auto it = Detail::Physics2D::Access::Entities(*phys).find(*req.physicsSelectedBody);
                it != Detail::Physics2D::Access::Entities(*phys).end())
                selectedBody = it->second;
        // PlanPhysicsOverlay's rule (Source/Programs/ArcaneEditor/src/Scene/PhysicsOverlay.hpp:16-22):
        // the whole world when toggled, else the selected body in Edit mode, else nothing.
        const bool drawPhysics = req.physicsWholeWorld || (!req.playMode && selectedBody.has_value());
        const Manifold2D::Physics::PhysicsWorld* solver = Detail::Physics2D::Access::Solver(*phys);
        if (solver && drawPhysics)
        {
            if (const std::optional<Affine2D> affine = packet.spriteView->view.AsAffine2D())
            {
                OverlayBatchCollector collector(packet.overlay);
                collector.Begin(req.viewport.x, req.viewport.y);
                PhysicsDebugDrawOptions2D opts = MakePhysicsDebugDrawOptions();
                opts.view = *affine;
                // The same RunLoop alpha the interpolated sprites use (today's
                // opts.alpha = ctx->alpha, EditorAppFrame.cpp:1930).
                const RenderContext2D* rc = reg.GetResource<RenderContext2D>();
                opts.alpha = rc ? rc->alpha : 0.0f;
                opts.interp = reg.GetResource<PhysicsInterpBuffer2D>();
                opts.velocities = opts.comMarkers = opts.orientations = false;
                opts.contacts = req.physicsWholeWorld;
                if (!req.physicsWholeWorld && selectedBody)
                    opts.onlyBody = (static_cast<std::uint64_t>(selectedBody->index) << 32) | selectedBody->generation;
                DrawPhysicsDebug(*solver, collector, opts);
            }
        }
    }
}
if (req.cameraRect && req.editorView && !req.playMode)
{
    if (const std::optional<SceneCameraView> cam = ActiveSceneCamera(reg, req.viewport))
    {
        const float aspect = req.viewport.y > 0u ? float(req.viewport.x) / float(req.viewport.y) : 1.0f;
        const float hw = cam->halfHeight * aspect;
        const float hh = cam->halfHeight;
        const glm::vec2 c = cam->worldCenter;
        const glm::vec3 world[4] = {
            { c.x - hw, c.y - hh, 0.0f }, { c.x + hw, c.y - hh, 0.0f },
            { c.x + hw, c.y + hh, 0.0f }, { c.x - hw, c.y + hh, 0.0f },
        };
        glm::vec3 screen[4];
        bool finite = true;
        for (int i = 0; i < 4; ++i)
        {
            screen[i] = req.editorView->WorldToScreen(world[i]);
            finite = finite && std::isfinite(screen[i].x) && std::isfinite(screen[i].y);
        }
        if (finite)
        {
            for (int i = 0; i < 4; ++i)
            {
                OverlayLine line{};
                line.a = glm::vec3(glm::vec2(screen[i]), 0.0f);
                line.b = glm::vec3(glm::vec2(screen[(i + 1) % 4]), 0.0f);
                line.thickness = 1.0f;
                line.color = req.cameraFrameColor;
                line.depth = OverlayDepthClass::AlwaysOnTop;
                line.space = OverlayColorSpace::Linear;
                line.screenSpace = true;
                packet.overlay.lines.push_back(line);
            }
        }
    }
}
packet.overlay.lines.insert(packet.overlay.lines.end(), req.gizmoPrims.lines.begin(), req.gizmoPrims.lines.end());
packet.overlay.fills.insert(packet.overlay.fills.end(), req.gizmoPrims.fills.begin(), req.gizmoPrims.fills.end());
```

`SubmitSceneToBatcher` stops calling `DrawGrid2D` (`:1896`), `SubmitRender` (`:1900`), the physics `GetResource` block (`:1907-1937`), and `ActiveSceneCamera` (`:1963`). Task 3's collector calls in `SubmitSceneToBatcher`, `m_overlayPrims`, and the `vp.overlay = &m_overlayPrims` line in `ArmGraphViewportFrame` are deleted in the same edit. The editor call site that already calls `ExtractFrame` sets `showGrid2d`, `playMode = InPlayMode()`, `physicsDebug = true` in Edit and in Play (the editor viewport always asks; extract applies `PlanPhysicsOverlay`'s rule, because ArcaneRender cannot call that editor function, and draws through `packet.spriteView`, so an empty `editorView` in Play does not skip the overlay), `physicsWholeWorld = m_physicsOverlay`, `physicsSelectedBody = m_selection.Primary()` when `m_selection.HasSelection()` (editor state; the entity-to-body lookup is extract's), `cameraRect`, the grid style fields from `Settings<EditorGridSettings>()`, and `cameraFrameColor` from the theme value it reads today (`:1987-1989`). Gizmo lines are copied into `req.gizmoPrims` with the tags Task 3 already applied.

Delete the `vp.grid` assignment at `EditorAppFrame.cpp:2118-2129`. The same condition (not play mode, `ViewMode::Perspective`, `EditorViewportSettings::showGrid`, `MakeGridScene`, `view = m_runtime->View()`, `SetPlane` from `gridPlane`) moves onto the `ExtractRequest` inside `RenderSceneToViewport`, as `req.showGrid3d` and `req.grid3d`. Extract already copies those into `packet.overlay` in the block above. After this task the 3D grid reaches the node only as `packet.overlay.grid3d`.

`DeclareGraphFrame`, when `context->Packet()` is non-null, ignores `shape.overlay` and `shape.grid`. `wantOverlay` is then true when that packet's `overlay.lines` or `overlay.fills` is non-empty or `overlay.hasGrid3d` is set. The 3D-grid pointer passed to `AddOverlayNode` is `&packet->overlay.grid3d` only when `hasGrid3d` is set, and null otherwise. When `Packet()` is null, keep Task 3's `shape.overlay` / `shape.grid` test, so the GPU cases that set `frame.overlay` and never call `BindPacket` stay green. `BuildFrame` may still copy those two fields for that path. Do not delete `FrameDesc::overlay` or `FrameDesc::grid`.

`OverlayNode::Record` captures the `NriGraphContext*` the way `AddBatch2DNode` does (`Batch2DNode.cpp:1628-1641` in Task 5). When `Packet()` is non-null it draws `Packet()->overlay` (lines, fills, and `grid3d` when `hasGrid3d`) and does not read the shape pointers. When `Packet()` is null it draws the pointers `AddOverlayNode` was given. A host frame that binds a packet and leaves `FrameDesc::overlay` and `FrameDesc::grid` null still draws the packet's overlay. The backbuffer case fails if this branch is missing.

The draw-time `GetComponent` already left the panel lambda in Task 3, into `EditorApp::BuildGizmoOverlayPrims` (not a host-gate function), which runs before `RenderSceneToViewport` and writes `m_gizmoPrims`. The request builder assigns `req.gizmoPrims = m_gizmoPrims` and does not call `GetComponent`. Interaction reads at `:1250`, `:1292`, and `:1348` stay in their current functions.

- [ ] **Step 4: Run the overlay case and the editor grid tests.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "overlay tags"
.\ArcaneTests.exe "a selected body outlines only that body"
.\ArcaneTests.exe "extracted overlay reaches the backbuffer"
.\ArcaneTests.exe "[editor][grid]"
```

Expected: PASS. The selected-body case draws lines only on the selected box's side, none with no selection and the toggle off, both sides with the toggle on, both sides in Play with the toggle on, and nothing in Play with only a selection. `[editor][grid]` is `ViewportGridTest.cpp`, still drawing through `Editor::DrawGrid2D`. The backbuffer case fails if the pass still reads only `FrameDesc`.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Client/ArcaneRender/src/Arcane/Render/Grid2D.hpp Source/Client/ArcaneRender/src/Arcane/Render/Grid2D.cpp Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/NriGraphContext.cpp Source/Client/ArcaneRender/src/Arcane/Render/Nri/nodes/OverlayNode.cpp Source/Programs/ArcaneEditor/src/Viewport/ViewportGrid.hpp Source/Programs/ArcaneEditor/src/Viewport/ViewportGrid.cpp Source/Programs/ArcaneEditor/src/App/EditorApp.hpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/ExtractFrameTest.cpp Tests/ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): extract publishes overlay primitives" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 10: UI snapshot, post chain, and the default light

**Files:**
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp`
- Modify: `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp:58-110`
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp:681-686` and the `req` fill from Task 5
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` (`ApplyDefaultLight` at `:2099`, `RefreshSceneResolution` at `:1581`, the game-HUD block at `:2059-2065` moves from `ArmGraphViewportFrame` into `RenderSceneToViewport`)
- Modify: `Source/Programs/ArcaneEditor/src/App/EditorApp.hpp` (`ImDrawData* m_gameUiDrawData`)
- Modify: `Tests/ArcaneTests/src/NriGraphPixelTest.cpp` (the packet game-HUD witness)
- Test: `Tests/ArcaneTests/src/ExtractFrameTest.cpp`

**Interfaces:**
- Consumes: `CopyImDrawSnapshot` and `ImDrawSnapshotRebuild` from Task 4 (ArcaneImGui), and the `RenderOverlayHooks::snapshot` member Task 4 added, which is how ArcaneRender's `ExtractFrame` reaches the copy (ruling PR-1). The editor and runtime hosts install it through `InstallImGuiHostHooks` (`Source/Programs/ArcaneEditor/src/main.cpp:339`, `Source/Programs/ArcaneRuntimeHost/src/RuntimeHost.cpp:67`), and so does ArcaneTests (`Tests/ArcaneTests/src/test_main.cpp:25`), which this task's test relies on. The ImGui node lives in ArcaneImGui (`Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp`) and the vehicle hands it out as a `RenderOverlayNode*` (`NriGraphContext.hpp:1041`, `:1049`). `ImGuiNriNode::Record` (`ImGuiNriNode.cpp:58`) calls `RenderDrawData`. `AddImGuiNode` (`:63`) reads `CurrentImGuiDrawData()` / `CurrentGameUiDrawData()` at `:88` and `:110`. `ApplyDefaultLight` (`MeshNode.hpp:363`). `PostChainDesc` is copied by value; its bytecode members are `std::shared_ptr<const std::vector<std::uint8_t>>` (`PostChainDesc.hpp`, moved in Task 4 from `PostChainCache.hpp:56-79`).
- Produces: `packet.imgui`, `packet.gameUi`, `packet.post` / `packet.hasPost`, and the three light vectors.

- [ ] **Step 1: Write the failing test.**

`ImGui::Render` is what builds the source. Do not hand-fill a list and call `AddDrawList`: that asserts (`imgui_draw.cpp:2305-2308`). `VtxOffset > 0` is Task 4's case. This case checks two real windows, the raw user-texture ids, the texture-pointer list, the post bytecode, and a callback refusal through `AddCallback` on a second rendered frame. The host batcher is installed so the sprite copy from Task 5 does not depend on a null context. `World` has no sprites here.

```cpp
static void DummyImGuiCallback(const ImDrawList*, const ImDrawCmd*) {}

TEST_CASE("extract copies two ImGui lists and keeps the post bytecode", "[nri][extract]")
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(32.0f, 32.0f);
    // Before NewFrame, matching the live backend (ImGuiNri.cpp:119):
    // without RendererHasTextures NewFrame demands a built font atlas
    // (imgui_draw.cpp:2778).
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    // First-frame windows are hidden unless both sizes are API-set
    // (imgui.cpp:8093-8095, :6394-6395); see Task 4's case.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(32.0f, 32.0f), ImGuiCond_Always);
    ImGui::Begin("one");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(42), ImVec2(0.0f, 0.0f), ImVec2(8.0f, 8.0f));
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(32.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(32.0f, 32.0f), ImGuiCond_Always);
    ImGui::Begin("two");
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(7), ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f));
    ImGui::End();
    ImGui::Render();
    ImDrawData* live = ImGui::GetDrawData();
    REQUIRE(live != nullptr);
    REQUIRE(live->CmdLists.Size >= 2);
    REQUIRE(live->Textures != nullptr);

    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 1, 2, 3, 4 });
    Arcane::PostChainDesc desc;
    desc.passes.push_back({});
    desc.passes.back().vsBytes = bytes;

    World world;
    auto batcher = Arcane::Batcher2D::Create();
    batcher->Begin(32u, 32u);
    Arcane::RenderContext2D renderCtx;
    renderCtx.batcher = batcher.get();
    world.reg.SetResource<Arcane::RenderContext2D>(renderCtx);

    Arcane::PacketRing ring;
    Arcane::ExtractRequest req;
    req.viewport = { 32u, 32u };
    req.imgui = live;
    req.post = &desc;
    const Arcane::RenderFramePacket& packet = Arcane::ExtractFrame(world.reg, req, ring);

    REQUIRE(packet.imgui.lists.size() == static_cast<std::size_t>(live->CmdLists.Size));
    REQUIRE(packet.imgui.lists.size() >= 2u);
    CHECK(packet.imgui.hasTextures);
    REQUIRE(packet.imgui.textures.size() == static_cast<std::size_t>(live->Textures->Size));
    bool saw42 = false;
    bool saw7 = false;
    for (std::size_t li = 0; li < packet.imgui.lists.size(); ++li)
    {
        CHECK(packet.imgui.lists[li].vertices.size() ==
              static_cast<std::size_t>(live->CmdLists[static_cast<int>(li)]->VtxBuffer.Size));
        for (const Arcane::ImDrawListSnapshot::Cmd& cmd : packet.imgui.lists[li].cmds)
        {
            if (cmd.managed == nullptr && cmd.texture == static_cast<ImTextureID>(42))
                saw42 = true;
            if (cmd.managed == nullptr && cmd.texture == static_cast<ImTextureID>(7))
                saw7 = true;
        }
    }
    CHECK(saw42);
    CHECK(saw7);
    CHECK(packet.hasPost);
    desc = {};
    bytes.reset();
    REQUIRE(packet.post.passes.size() == 1u);
    REQUIRE(packet.post.passes[0].vsBytes);
    CHECK(packet.post.passes[0].vsBytes->size() == 4u);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(32.0f, 32.0f), ImGuiCond_Always);
    ImGui::Begin("callback");
    ImGui::GetWindowDrawList()->AddCallback(&DummyImGuiCallback, nullptr);
    ImGui::End();
    ImGui::Render();
    Arcane::ExtractRequest req2;
    req2.viewport = { 32u, 32u };
    req2.imgui = ImGui::GetDrawData();
    Arcane::PacketRing ring2;
    const Arcane::RenderFramePacket& refusedPacket = Arcane::ExtractFrame(world.reg, req2, ring2);
    CHECK(refusedPacket.imgui.lists.empty());
    CHECK_FALSE(refusedPacket.imgui.hasTextures);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
}
```

The editor ordering is pinned in `ExtractFrameTest.cpp` (no plugin in the tree draws a game HUD -- `ReferenceGame` has no `OnDrawUI` -- so an editor witness in Play would render an empty HUD on today's tree as well; the GPU case below is the pixel witness):

```cpp
static std::string MemberBody(const std::string& text, const char* signature)
{
    const std::size_t fn = text.find(signature);
    REQUIRE(fn != std::string::npos);
    const std::size_t end = text.find("\n    }\n", fn);
    REQUIRE(end != std::string::npos);
    return text.substr(fn, end - fn);
}

TEST_CASE("the editor builds its game HUD before extract and hands it to the request", "[nri][extract]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::string render = MemberBody(text, "void EditorApp::RenderSceneToViewport(");
    const std::size_t hud = render.find("RenderToDrawData()");
    const std::size_t extract = render.find("ExtractFrame(");
    REQUIRE(hud != std::string::npos);
    REQUIRE(extract != std::string::npos);
    CHECK(hud < extract);
    CHECK(render.find("req.gameUi = m_gameUiDrawData;") != std::string::npos);
    const std::string arm = MemberBody(text, "void EditorApp::ArmGraphViewportFrame(");
    CHECK(arm.find("RenderToDrawData()") == std::string::npos);
    CHECK(arm.find("vp.gameUi = m_gameUiDrawData;") != std::string::npos);
}
```

Append the Play-HUD pixel witness to `NriGraphPixelTest.cpp`, after Task 4's `CheckImGuiSnapshotReachesRenderer` (same helpers and includes). A bound packet whose `gameUi` carries a red rect, and a live `FrameDesc::gameUi` that is empty (it only declares the `gameui` node), must produce the red rect: the HUD node reads the packet, so the Play HUD is not empty. `NodeSet::gameUi` must be set or `ImGuiGame()` is null (`NriGraphContext.hpp:373-378`).

```cpp
namespace
{
    void CheckPacketGameUiReachesTheHud(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();

        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const game = ImGui::CreateContext();
        REQUIRE(game != nullptr);
        ImGui::SetCurrentContext(game);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        io.DeltaTime = 1.0f / 60.0f;

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.gameUi = true;
        PixelVehicle v = MakeVehicle(backend, nodes);
        Arcane::RenderOverlayNode* const hud = v.ctx->ImGuiGame();
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(game);

        // The Play HUD the editor built before extract.
        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(70.0f, 40.0f), ImVec2(90.0f, 56.0f), IM_COL32(255, 0, 0, 255));
        ImGui::Render();
        Arcane::RenderFramePacket packet;
        REQUIRE(Arcane::CopyImDrawSnapshot(ImGui::GetDrawData(), packet.gameUi));
        REQUIRE(packet.gameUi.hasTextures);

        // An empty live frame: it declares the gameui node and draws nothing itself.
        ImGui::NewFrame();
        ImGui::Render();
        Arcane::NriGraphContext::FrameDesc frame;
        frame.gameUi = ImGui::GetDrawData();
        frame.capture = true;
        v.ctx->BindPacket(&packet);
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        REQUIRE(w == kW);
        REQUIRE(h == kH);
        const Rgba px = At(rgba, w, 80u, 48u);
        CHECK(px.r > px.g + 60);
        CHECK(px.r > px.b + 60);
        CHECK(Arcane::RenderErrorCount() == before);

        v.ctx.reset();
        ImGui::DestroyContext(game);
        ImGui::SetCurrentContext(previous);
    }
}

TEST_CASE("a bound packet's game HUD reaches the gameui node (d3d12)", "[gpu][packet][imgui][nri][d3d12]")
{
    CheckPacketGameUiReachesTheHud(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("a bound packet's game HUD reaches the gameui node (vulkan)", "[gpu][packet][imgui][nri][vulkan]")
{
    CheckPacketGameUiReachesTheHud(Arcane::GraphicsBackend::Vulkan);
}
```

`PostChainPassDesc::vsBytes` is the `shared_ptr<const vector<uint8_t>>` (`PostChainCache.hpp:58`, moved to `PostChainDesc.hpp` in Task 4). The copy is the struct assignment `packet.post = *req.post`.

- [ ] **Step 2: Run the case.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract copies two ImGui lists"
```

Expected: FAIL. `packet.imgui.lists` is empty and `hasPost` is false.

- [ ] **Step 3: Copy the snapshot, the chain, and the light.**

Inside `ExtractFrame`, after the overlay block:

```cpp
// Ruling PR-1: ArcaneRender cannot call ArcaneImGui directly; the copy is the
// overlay hook. No hook (a host without ImGui) leaves both snapshots empty.
const RenderOverlayHooks overlayHooks = GetRenderOverlayHooks();
if (!overlayHooks.snapshot || !overlayHooks.snapshot(req.imgui, packet.imgui))
    packet.imgui = {};
if (!overlayHooks.snapshot || !overlayHooks.snapshot(req.gameUi, packet.gameUi))
    packet.gameUi = {};
packet.hasPost = req.post != nullptr;
if (req.post)
    packet.post = *req.post;
packet.lightDirection = req.lightDirection;
packet.lightColor = req.lightColor;
packet.ambient = req.ambient;
```

`RuntimeFrame`, where it fills `req`, before `ExtractFrame`:

```cpp
Arcane::MeshSceneDesc light{};
Arcane::ApplyDefaultLight(light, Arcane::Settings<Arcane::RenderMeshDefaultLightSettings>());
req.lightDirection = light.lightDirection;
req.lightColor = light.lightColor;
req.ambient = light.ambient;
req.imgui = graphFrame.imgui;
req.gameUi = graphFrame.gameUi;
req.post = graphFrame.post;
```

`graphFrame.imgui` is assigned at `:510`. `graphFrame.post` is `const PostChainDesc*` (`NriGraphContext.hpp:458`), assigned at `RuntimeFrame.cpp:697`. `graphFrame.gameUi` is the `FrameDesc` field at `NriGraphContext.hpp:547`; `RuntimeFrame` leaves it null. Delete the `ApplyDefaultLight` call inside the mesh-scene block Task 6 moved to after `ExtractFrame`. That block copies `packet.lightDirection`, `packet.lightColor`, and `packet.ambient` onto the mesh desc. The editor deletes `ApplyDefaultLight` at `:2099` and fills the same three request fields at its `ExtractFrame` call, and `ArmGraphViewportFrame` copies `lightDirection`, `lightColor`, and `ambient` from `m_packetRing.Latest()` onto `m_meshScene` where that call was, so a live `render.mesh.defaultLight.*` change still reaches the editor's mesh pass.

**The editor's game HUD enters the packet.** Today `ArmGraphViewportFrame` produces it (`vp.gameUi = m_gameImgui->RenderToDrawData()` behind `BeginGameUiFrame()`, `EditorAppFrame.cpp:2059-2065`), which runs after the `ExtractFrame` call Task 5 placed after `SubmitSceneToBatcher`, so `req.gameUi` would be null and the `gameui` node would draw an empty `packet.gameUi` in Play. Move that block into `RenderSceneToViewport`, after `SubmitSceneToBatcher(b)` and before `ExtractFrame`: `m_gameUiDrawData = BeginGameUiFrame() ? m_gameImgui->RenderToDrawData() : nullptr;` (an `ImDrawData*` member in `EditorApp.hpp`), then `req.gameUi = m_gameUiDrawData;` on the request `BuildExtractRequest` returned. `ArmGraphViewportFrame` only assigns `vp.gameUi = m_gameUiDrawData;` so the node is still declared from it. The HUD is still produced after the scene submission (the rule the comment at `EditorAppFrame.cpp:2046-2048` states; update that comment, which also says the call cannot move earlier, to say it now runs before extract and still after `SubmitSceneToBatcher`), and a plugin `DrawUI` that edits the scene still lands this frame, because sprite submission now runs inside the extract that follows. The scene-save cover never builds a HUD and leaves `req.gameUi` null. Writing `Latest().gameUi` after arming instead would be a second post-extract writer, which spec 4.1 and the `Latest()` rule in Task 4 do not allow.

`AddImGuiNode`'s record lambda (`ImGuiNriNode.cpp:105-114`) becomes:

```cpp
[context, target, gameUi](RenderGraphNodeContext& nodeContext)
{
    if (!context)
        return;
    if (RenderOverlayNode* node = gameUi ? context->ImGuiGame() : context->ImGuiHud())
    {
        if (context->Packet())
        {
            const ImDrawSnapshot& snap = gameUi ? context->Packet()->gameUi : context->Packet()->imgui;
            ImDrawSnapshotRebuild rebuilt(snap);
            node->Record(nodeContext, rebuilt.Data(), target);
        }
        else
        {
            node->Record(nodeContext,
                         gameUi ? context->CurrentGameUiDrawData()
                                : context->CurrentImGuiDrawData(),
                         target);
        }
    }
}
```

The declaration-time block at `:80-92` builds its own stack `ImDrawSnapshotRebuild` when `context->Packet()` is set, and keeps the live pointers when it is not. That object is not the record lambda's rebuild. Both name the same `ImTextureData` objects, so `NewFrameTexUpdates` (`ImGuiNri.cpp:772-779`) can `SetTexID` before `RenderDrawData` reads `GetTexID`. A null `Textures` makes `NewFrameTexUpdates` return immediately (`:775-776`).

```cpp
if (context)
{
    if (RenderOverlayNode* node = gameUi ? context->ImGuiGame() : context->ImGuiHud())
    {
        if (context->Packet())
        {
            const ImDrawSnapshot& snap = gameUi ? context->Packet()->gameUi : context->Packet()->imgui;
            ImDrawSnapshotRebuild rebuilt(snap);
            node->PrepareFrame(rebuilt.Data(), context->Graves(), context->Graph().DebugSubmitCount());
        }
        else
        {
            node->PrepareFrame(gameUi ? context->CurrentGameUiDrawData()
                                      : context->CurrentImGuiDrawData(),
                               context->Graves(),
                               context->Graph().DebugSubmitCount());
        }
    }
}
```

A preview graph has no packet and keeps the live pointers. `ImGuiNriNode.cpp` (ArcaneImGui) includes `<Arcane/ImGui/ImDrawSnapshot.hpp>` for `ImDrawSnapshotRebuild` and `<Arcane/Render/RenderFramePacket.hpp>` for `Packet()`'s type; the node variable is the `RenderOverlayNode*` the vehicle returns, exactly as the lambdas declare it today (`ImGuiNriNode.cpp:82`, `:109`). `ExtractFrame.cpp` includes `<Arcane/Render/Nri/RenderOverlayNode.hpp>` for `GetRenderOverlayHooks`. Service stays here, at declaration. Extract does not call `NewFrameTexUpdates` and does not wait on the graphics queue (`ImGuiNri.hpp:185-205`).

`RefreshSceneResolution` deletes the `ActiveSceneCamera` branch at `:1581-1586`. Play mode leaves the runtime view for extract: the editor's `ExtractFrame` call sets `req.playMode = InPlayMode()` and leaves `req.editorView` empty on that path, and immediately after `ExtractFrame` returns calls `SetView(packet.spriteView->view)` when `packet.spriteViewFromScene` is true, so a scene without a camera keeps the plugin's own view (today's fall-through, `EditorAppFrame.cpp:1574-1576`). The Play path still fills `physicsDebug`, `physicsWholeWorld`, and `playMode`; Task 9's physics block draws through `packet.spriteView`, so the empty `editorView` does not skip the overlay. Edit mode keeps `SetView(m_camera.Resolve(ViewportSize()))` at `:1579` and passes that same value as `req.editorView`. `EditorApp::RefreshSceneResolution(` is already a host-gate signature; after this edit its body contains none of the forbidden tokens.

Thumbnail `Impl::Harvest` and `RenderGraphPreview` do not gain `ExtractFrame` or a registry read.

- [ ] **Step 4: Run the UI case.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "extract copies two ImGui lists"
.\ArcaneTests.exe "the editor builds its game HUD before extract"
.\ArcaneTests.exe "[packet]"
```

Expected: PASS. `[packet]` includes the two `"a bound packet's game HUD reaches the gameui node"` cases, which read a red-dominant pixel at `(80, 48)`. `[packet]` still covers `ImDrawSnapshotRebuild` from Task 4, including the two `[gpu][packet]` reach cases, which stay red-dominant.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Programs/ArcaneEditor/src/App/EditorApp.hpp Tests/ArcaneTests/src/NriGraphPixelTest.cpp Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp Tests/ArcaneTests/src/ExtractFrameTest.cpp
git commit -m "feat(render): extract publishes the UI snapshot and the post chain" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 11: Behavioural extract-skip on the three hosts

**Files:**
- Modify: `Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp` (one `fprintf` immediately before the `if (!m_config.reportPath.empty())` at `:1270`, which sits inside `ShutdownGraphPath`'s `#if defined(ARC_RUNTIME_HOST_VERIFY)` region, `:1186-1564`)
- Modify: `Source/Programs/ArcaneServer/src/ServerReport.hpp` (one field; `kSchemaVersion` stays 1)
- Modify: `Source/Programs/ArcaneServer/src/ServerReport.cpp:25` (`ToJson`)
- Modify: `Source/Programs/ArcaneServer/src/ServerApp.cpp:225-230` (assign the field before `Finish`)
- Test: `Tests/ArcaneTests/src/ExtractHostTest.cpp`

**Interfaces:**
- Consumes: `ExtractFrameCallCount` / `ResetExtractFrameCallCount` from Task 5. `RunLoop::Advance(double)` (`RunLoop.hpp:109`). `Arcane::Runtime rt(Arcane::Test::Process())` (`ClientRuntimeTest.cpp:28`). `RuntimeApp --headless` still builds an offscreen vehicle (`RuntimeApp.cpp:451-461`, built at `:502-581` under `ARC_RUNTIME_HOST_VERIFY`). `WitnessScratch` / `RunWitness` (`HostWitness.hpp`, the pattern in `ServerWitnessTest.cpp:27-54`). `ServerApp::Run` ticks `Loop().Advance` (`ServerApp.cpp:191`) and sets `hasRenderSubmission = false` (`:230`). ArcaneServer links ArcaneEngine, ArcaneNet, and ArcaneCrypto (`premake5.lua:884-887`); their closure includes ArcaneCore, where the counter lives, and no Client module. `Arcane::Platform::ExecutableFileName` (`<Arcane/Platform/Platform.hpp>`) names a host binary on every platform (`ServerWitnessTest.cpp:5`, `:19`).
- Produces: `ServerReport::extractFrameCalls`, the stdout line `extract-frames: N`, and three tests. No source scan for the token `ExtractFrame`.

- [ ] **Step 1: Write the failing tests.**

`Tests/ArcaneTests/src/ExtractHostTest.cpp`:

```cpp
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/ExtractAccounting.hpp>   // ArcaneCore
#include <Arcane/Base/Runtime.hpp>             // ArcaneEngine
#include <Arcane/Platform/Platform.hpp>        // ExecutableFileName: .exe on Windows only
#include "Helpers/TestTypeContext.hpp"
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("a non-rendering runtime leaves the extract counter at 0", "[nri][extract]")
{
    Arcane::ResetExtractFrameCallCount();
    Arcane::Runtime rt(Arcane::Test::Process());
    for (int i = 0; i < 4; ++i)
        rt.Loop().Advance(1.0 / 60.0);
    CHECK(Arcane::ExtractFrameCallCount() == 0u);
}

static std::string Slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int CountExactLine(const std::string& text, const std::string& want)
{
    int n = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        std::size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == want) ++n;
        i = end + (end < text.size() ? 1 : 0);
        if (end == text.size()) break;
    }
    return n;
}

TEST_CASE("RuntimeApp --headless prints extract-frames equal to --frames", "[gpu][extract][witness]")
{
    const std::filesystem::path dir = std::filesystem::absolute("../ArcaneRuntime");
    REQUIRE(std::filesystem::exists(dir / Arcane::Platform::ExecutableFileName("ArcaneRuntime")));
    Arcane::Test::WitnessScratch scratch(dir, "extract-headless");
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneRuntime");
    inv.workingDir = scratch.Dir();
    inv.args = { "--project", "ReferenceProject", "--frames", "4", "--headless" };
    inv.hardCapMs = 60000;
    Arcane::Test::WitnessRun run = Arcane::Test::RunWitness(inv);
    INFO(Slurp(run.stdoutPath));
    CHECK(run.exitCode == 0);
    CHECK(CountExactLine(Slurp(run.stdoutPath), "extract-frames: 4") == 1);
}

TEST_CASE("ArcaneServer ticks N frames with extractFrameCalls 0", "[witness][server][extract]")
{
    const std::filesystem::path dir = std::filesystem::absolute("../ArcaneServer");
    REQUIRE(std::filesystem::exists(dir / Arcane::Platform::ExecutableFileName("ArcaneServer")));
    Arcane::Test::WitnessScratch scratch(dir, "extract-server");
    Arcane::Test::WitnessInvocation inv;
    inv.exePath = scratch.Dir() / Arcane::Platform::ExecutableFileName("ArcaneServer");
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "server-report.json";
    inv.args = { "--project", "ReferenceProject", "--frames", "4", "--report", inv.reportPath.generic_string() };
    inv.hardCapMs = 60000;
    Arcane::Test::WitnessRun run = Arcane::Test::RunWitness(inv);
    REQUIRE(run.exitCode == 0);
    CHECK(run.report.at("framesTicked") == 4);
    CHECK(run.report.at("extractFrameCalls") == 0);
    CHECK(run.report.at("systems").at("hasRenderSubmission") == false);
    CHECK(run.report.at("systems").at("render") == 0);
    CHECK(run.report.at("presentation").at("clientAttached") == false);
    CHECK(run.report.at("presentation").at("clientDllLoadedAtBoot") == false);
    CHECK(run.report.at("schemaVersion") == 1);
}
```

Use the real namespaces from `HostWitness.hpp` (`Arcane::Test` is what `ServerWitnessTest.cpp` imports via `using namespace Arcane::Test`). Match `WitnessInvocation::args` to that header's element type.

- [ ] **Step 2: Build and run the three cases.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "leaves the extract counter"
.\ArcaneTests.exe "prints extract-frames"
.\ArcaneTests.exe "extractFrameCalls"
```

Expected: the in-process case PASSes (the counter exists and nothing in that process calls `ExtractFrame`). The two witness cases FAIL: stdout has no `extract-frames: 4` line, and the server JSON has no `extractFrameCalls` key.

- [ ] **Step 3: Report the counter from the rendering host and from the server.**

Immediately before `if (!m_config.reportPath.empty())` at `RuntimeApp.cpp:1270`:

```cpp
std::fprintf(stdout, "extract-frames: %llu\n",
             static_cast<unsigned long long>(Arcane::ExtractFrameCallCount()));
std::fflush(stdout);
```

This is `fprintf`, not `ARC_INFO`. It runs when `reportPath` is empty. It is inside the `ARC_RUNTIME_HOST_VERIFY` region, so ArcaneRuntime.exe prints it and a monolithic package (which refuses `--headless` and `--report`) does not; the witness runs the staged ArcaneRuntime.exe. `VerifyReport::schemaVersion` stays 14.

`ServerReport` gains `std::uint64_t extractFrameCalls = 0;`. `ToJson` writes `j["extractFrameCalls"] = extractFrameCalls;`. `kSchemaVersion` stays 1. `ServerApp::Run`, beside the other `rep.` assignments at `:225-230` and before `Finish`:

```cpp
rep.extractFrameCalls = Arcane::ExtractFrameCallCount();
```

A future `ExtractFrame` call inside the server tick would store N and fail the test. The existing named-key checks in `ServerWitnessTest.cpp:40-54` stay valid because the new key is additive.

- [ ] **Step 4: Re-run the three cases.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "leaves the extract counter"
.\ArcaneTests.exe "prints extract-frames"
.\ArcaneTests.exe "extractFrameCalls"
```

Expected: PASS. The headless runtime line equals `extract-frames: 4` once. The server report has `extractFrameCalls == 0` and `framesTicked == 4`.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Programs/ArcaneRuntimeHost/src/RuntimeApp.cpp Source/Programs/ArcaneServer/src/ServerReport.hpp Source/Programs/ArcaneServer/src/ServerReport.cpp Source/Programs/ArcaneServer/src/ServerApp.cpp Tests/ArcaneTests/src/ExtractHostTest.cpp
git commit -m "test(render): extract counter stays zero on non-rendering hosts" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

---

### Task 12: The include gate and the host frame-builder gate

**Files:**
- Create: `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp`
- Modify: `Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneResources.hpp` (include the new header; delete the moved definitions)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/MeshCache.hpp:54`, `MeshMaterialCache.hpp:59`, `SpriteCache.hpp:33` (include `SceneCacheRecords.hpp` instead of `SceneResources.hpp`)
- Modify: `Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.hpp:50` (the forward-declaration comment names the new header)
- Not modified (rev 5): Render's `PhysicsDebugDraw.cpp`. On `main` it includes only `PhysicsDebugDraw.hpp` and `Batcher2D.hpp` (`PhysicsDebugDraw.cpp:1-2`); the `SceneResources.hpp` include rev 4.1 retargeted at `PhysicsDebugDraw.cpp:37` moved with the physics-debug GATHER half to `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsDebugGeometry.cpp:20`, which is outside the render tree and outside this gate (ruling PR-2).
- Modify: `Tests/ArcaneTests/src/ArcaneSpellingGuardTest.cpp:34-55` (add `"Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp"` to `kGameFacingHeaders`, directly after the `SceneResources.hpp` entry, and update the count from 20 to 21 in the comments at `:12` and `:62` and in the case name at `:218`, `"guard: the 21 game-facing engine headers ..."`)
- Test: `Tests/ArcaneTests/src/RenderRegistryIncludeTest.cpp`
- Tasks 5–10 already removed the other registry includes this gate depends on, including Task 8's drops from `PickEmit.cpp` (`PhysicsComponents.hpp` at `:7` and `SceneResources.hpp` at `:9` among them). The three cache TUs are this task's split; they and the already-clean `PhysicsDebugDraw.cpp`, plus ArcaneImGui's `ImGuiNriNode.cpp` (the render node that left the render tree, (ruling PR-2)), are the per-TU list, named here, not files the compiler output picks. If the gate fails on a path that is absent from the table below, stop and correct this table before calling the task done.

**Interfaces:**
- Consumes: `ExtractFrame.cpp` as the one Render `.cpp` that includes `Astra/Registry/Registry.hpp`. `DepthConvention.hpp` (no registry). `SceneCamera.hpp:20` (includes `Registry.hpp`). The closed set below. `scripts/settings-compile-fail.ps1:11-14` (vswhere, then `VC\Auxiliary\Build\vcvars64.bat`). `SceneResources.hpp` includes `Ecs.hpp` at `:7`, and `Ecs.hpp:24` includes `Astra/Registry/Registry.hpp`. `Ecs.hpp:35` is `using Entity = Astra::Entity`. `Astra::Entity` is `Astra/Entity/Entity.hpp:160`.
- Produces: `SceneCacheRecords.hpp` and `[registry-gate]`. The compiler is an absolute `cl.exe` resolved in the test, so the case passes when the process cwd is the ArcaneTests exe dir.

`MeshCache.hpp:54`, `MeshMaterialCache.hpp:59`, and `SpriteCache.hpp:33` include `SceneResources.hpp` today, so each of those TUs preprocesses `Registry.hpp` (through `SceneResources.hpp:7` -> `Ecs.hpp:24`). Nothing in Tasks 5–10 edits those three files. Render's `PhysicsDebugDraw.cpp` no longer does: its only includes are `PhysicsDebugDraw.hpp` and `Batcher2D.hpp`, and `PhysicsDebugDraw.hpp` reaches `PhysicsDebugSettings.hpp` -> `Arcane/Scene/PhysicsDebugTypes.hpp` -> `Arcane/Config/Settings.hpp`, `ViewTransform.hpp`, and `Arcane/Scene/PhysicsDebugGeometry.hpp`, none of which include `Ecs.hpp` or `Registry.hpp`. Their other includes do not: `MeshCache.cpp` includes `MeshCache.hpp`, `Log.hpp`, and `MeshAsset.hpp`; `SpriteCache.cpp` includes `SpriteCache.hpp`, `Assets.hpp`, `Log.hpp`, and `SpriteAsset.hpp`; `MeshMaterialCache.cpp` includes `MeshMaterialCache.hpp`, `Log.hpp`, `MaterialAsset.hpp`, `MaterialSource.hpp`, and `MaterialTypes.hpp`; None of those headers include `Ecs.hpp` or `Registry.hpp`. The types the three cache TUs need from `SceneResources.hpp` are `MeshEntry`, `ResolvedMeshMaterial`, and `SpriteEntry`. The physics interpolation records (`PhysicsInterpBuffer2D`, `PhysicsInterpPose2D`, `PhysicsInterpSlot2D`, `Detail::Physics2D::Lerp`, `Detail::Physics2D::AngleLerp`) still move with them, as rev 4.1 specified for the debug-draw TU: after the split no gated TU needs them, and because `SceneResources.hpp` includes `SceneCacheRecords.hpp`, every consumer that reaches them through `SceneResources.hpp` (`RenderSystems.hpp`, `PhysicsSystem.hpp`, `PhysicsDebugGeometry.cpp`) compiles unchanged. `SpriteEntry`, `MeshEntry`, and `ResolvedMeshMaterial` use `Guid`, `glm`, `MeshData`, `MeshBounds`, `MeshSlot`, `MeshSource`, `MaterialBlendMode`, and `std::tuple`. `MeshAsset.hpp` includes `MeshBuilder.hpp` (`MeshData` at `:69`, `using MeshBounds = Aabb` at `:115`) and `ArtifactReader.hpp`. `MaterialBlendMode.hpp` includes only `<cstdint>`. `Guid.hpp`, `Aabb.hpp`, `ArtifactReader.hpp`, and `Constant.hpp` do not include `Ecs.hpp` or `Registry.hpp`.

`PhysicsInterpBuffer2D::slotOf` is `Astra::FlatMap<Arcane::Entity, PhysicsInterpSlot2D>` (`SceneResources.hpp:104`), inside the `// ARC_INTERNAL_BEGIN` / `// ARC_INTERNAL_END` fence at `:103`/`:105` that `ArcaneSpellingGuardTest.cpp` exempts (`SceneResources.hpp` is one of its `kGameFacingHeaders`, `:34-55`). `FlatMap.hpp` includes `Entity.hpp`, `Base.hpp`, `Simd.hpp`, `Memory.hpp`, and `Swiss.hpp` (`FlatMap.hpp:17-21`). Those headers, and the Mosaic headers they reach, do not include `Registry.hpp` or `Ecs.hpp`. The new header includes that one Astra container header and does not include `Ecs.hpp`. The moved field is spelled `Astra::FlatMap<Astra::Entity, PhysicsInterpSlot2D>` and keeps its fence lines. `Astra::Entity` is the same type as `Arcane::Entity`. Do not pimpl it: that would change the layout. This is not an ECS-registry include, and the per-TU check below asserts `Registry.hpp` and `Ecs.hpp` are both absent. `SceneCacheRecords.hpp` is game-facing (`SceneResources.hpp` includes it), so it joins `kGameFacingHeaders` in `ArcaneSpellingGuardTest.cpp`; the fenced `Astra::FlatMap<Astra::Entity, ...>` field stays scanned and exempt, exactly as it was inside `SceneResources.hpp`.

Move these definitions into `SceneCacheRecords.hpp` verbatim, including their comments: `Lerp` and `AngleLerp`, with the `namespace Detail::Physics2D` that now wraps them (`SceneResources.hpp:34-64`, comment included), `PhysicsInterpPose2D` (`:66-73`), `PhysicsInterpSlot2D` (`:75-84`), `PhysicsInterpBuffer2D` (`:86-110`, with the `slotOf` spelling above), `SpriteEntry` (`:132-144`), `MeshEntry` (`:208-250`), `ResolvedMeshMaterial` (`:292-332`). Leave `SceneRoot`, `RenderContext2D`, `SpriteTable`, `MeshTable`, and `MeshMaterialTable` in `SceneResources.hpp`. That header includes `SceneCacheRecords.hpp` and keeps `Ecs.hpp`, `Guid.hpp`, `ViewTransform.hpp`, `<unordered_map>`, and `<cstdint>`. It drops the includes only the moved types used: `FlatMap.hpp`, `<tuple>`, `<cmath>`, `Constant.hpp`, `MeshAsset.hpp`, `MeshBuilder.hpp`, `MaterialBlendMode.hpp`, `glm/glm.hpp`, and `<vector>`.

The new header includes `Constant.hpp`, `Guid.hpp`, `MaterialBlendMode.hpp`, `MeshAsset.hpp`, `FlatMap.hpp`, `glm/glm.hpp`, `<cmath>`, `<cstdint>`, `<tuple>`, and `<vector>`. It does not include `Ecs.hpp`, `Registry.hpp`, or `SceneResources.hpp`.

On each of the three includes, replace `SceneResources.hpp` with `SceneCacheRecords.hpp` and point the include-line comment at the new header. Also update the comments that name `SceneResources.hpp` as the home of a moved type: `MeshCache.hpp:39`, `SpriteCache.cpp:48` (the `SpriteEntry` field defaults; the old `:85-92` line numbers do not survive the move), and `PhysicsDebugDraw.hpp:50-53`. `PhysicsDebugDraw.hpp:54` stays a forward declaration (`struct PhysicsInterpBuffer2D;`). Comments that name `SceneResources.hpp` as the home of `SpriteTable` or `MeshTable` stay, because those tables stay. Do not edit host-function comments to satisfy the scanner; that is the next section.

Closed set. These are the only files the gate is allowed to talk about:

| File | What it includes after its owning task | Owner |
|---|---|---|
| `Source/Client/ArcaneRender/src/Arcane/Render/ExtractFrame.cpp` | `Astra/Registry/Registry.hpp`, and the headers that pull it in: `VisibilitySystem.hpp` (`:30`), `GpuSceneSync.hpp` (`:27`), `RenderSystems.hpp` (`:37`), `SceneCamera.hpp`, `PhysicsSystem.hpp` (via `Ecs.hpp:24`) | 5–10 |
| `Source/Client/ArcaneRender/src/Arcane/Render/PickEmit.cpp` | `PickEmit.hpp`, `Constant.hpp`, `glm/glm.hpp`, `<algorithm>`, `<cmath>`. Task 8 dropped `SpriteGeometry.hpp` (`:4`), `VisibilitySystem.hpp` (`:5`), `Components.hpp` (`:6`), `PhysicsComponents.hpp` (`:7`), `PhysicsSystem.hpp` (`:8`), `SceneResources.hpp` (`:9`), `Registry.hpp` (`:11`), and `<utility>` (`:17`). `PickPassId`, `PickSampleTexel`, `PickKindCode`, `PickBoundHalfExtents`, and `BuildPickIdGeometry` stayed. | 8 |
| `Source/Client/ArcaneRender/src/Arcane/Render/MeshCache.cpp` | `SceneCacheRecords.hpp` via `MeshCache.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| `Source/Client/ArcaneRender/src/Arcane/Render/MeshMaterialCache.cpp` | `SceneCacheRecords.hpp` via `MeshMaterialCache.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| `Source/Client/ArcaneRender/src/Arcane/Render/SpriteCache.cpp` | `SceneCacheRecords.hpp` via `SpriteCache.hpp`. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | 12 |
| `Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.cpp` | `PhysicsDebugDraw.hpp` and `Batcher2D.hpp` only. Not `SceneResources.hpp`, not `Ecs.hpp`, not `Registry.hpp`. | already true on `main` (rev 5) |
| `Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp` (ruling PR-2) | The render node that left the render tree with modularization. After Task 10: `ImDrawSnapshot.hpp`, `RenderFramePacket.hpp`, `NriGraphContext.hpp`. Not `Ecs.hpp`, not `Registry.hpp`. Compiled with ArcaneImGui's project flags. | 10 |
| Every other `.cpp` under `Source/Client/ArcaneRender/src/Arcane/Render/` | None of `Registry.hpp`, `VisibilitySystem.hpp`, `GpuSceneSync.hpp`, `RenderSystems.hpp`, `SceneCamera.hpp`, `Ecs.hpp`, `BoundsSystem.hpp`, `PhysicsSystem.hpp`. `MeshNode.cpp` mentions `SceneCamera.hpp` in comments and does not include it. | already true before Task 5 |

`GpuSceneHost.hpp:28` includes `Registry.hpp` and lives in ArcaneClient's `Host/`, outside this compile. `ClientRuntime.cpp` includes `RenderSystems.hpp` and lives in ArcaneClient's `Client/`, outside this compile. ArcanePhysics2D's `PhysicsDebugGeometry.cpp` (the gather half) and the rest of ArcaneImGui (`OffscreenImGuiLayer.cpp` reaches `Registry.hpp` through `Arcane/Sim/SimSettings.hpp` -> `RunLoop.hpp`) are outside it too (ruling PR-2). `ComponentRegistry.hpp` and `CVarRegistry.hpp` are different paths; the match string is `Astra/Registry/Registry.hpp` with either slash. `Nri/` has no allow-list entry.

Host-gate functions, and the only token each one may strip from a line before the forbidden-token scan:

| Signature | File | After the comment strip, erase this token from the line, then whole-word scan |
|---|---|---|
| `RenderGraph(` | `Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp:454` | `GpuSceneInvalidate` (the call at `:761`) |
| `EditorApp::RenderSceneToViewport(` | `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` | `GpuSceneInvalidate` (the call at `:1773`) |
| `EditorApp::ArmGraphViewportFrame(` | `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` | `PickPassIdOf` (the loop at `:2187-2191`) |
| `EditorApp::RefreshSceneResolution(` | `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp:1515` | nothing |
| `EditorApp::SubmitSceneToBatcher(` | `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` | nothing |
| `MaterialPreviewHarvester::Impl::Harvest(` | `Source/Programs/ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp:1138` | nothing |
| `RenderGraphPreview(` | `Source/Programs/ArcaneEditor/src/Documents/ShaderEditorDocument.cpp:1662` | nothing |
| `EditorApp::CaptureGraphViewportPng(` | `Source/Programs/ArcaneEditor/src/App/EditorApp.cpp:2056` (the scene-save cover; a frame builder since Task 5) | nothing |
| `EditorApp::BuildExtractRequest(` | `Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp` (Task 5's request builder) | nothing |

Strip `//` and `/* */` comments over the whole file before locating the function body (so a brace in a comment cannot move the body's end) and before the line scan, then apply the allow-list `Strip` of `GpuSceneInvalidate` or `PickPassIdOf`, then match forbidden tokens as whole words. An identifier character is `[A-Za-z0-9_]`. `GpuSceneSync` does not match `GpuSceneSyncNode`. `ActivePerspectiveSceneCamera` does not contain `ActiveSceneCamera` as a substring. Replacing a comment with a space, and keeping the newlines inside a block comment, stops two tokens from joining across it. A `//` or `/*` inside `"..."` or `'...'` is not a comment (a `\` escapes the next character); a forbidden token that sits inside a string still counts. No host source comment is edited to make this pass. The comments the stripper has to ignore, and that stay in the source, are: `RuntimeFrame.cpp:556` and `:579` (`CollectPickables`, inside `RenderGraph` at `:454`), `:656` and `:757` (`PrepareSceneForRender`), `:669` and `:756` (`GpuSceneSyncNode`); `EditorAppFrame.cpp:1573` (`ActiveSceneCamera`, inside `RefreshSceneResolution` at `:1515`), `:1688` (`GpuSceneSync`, inside `RenderSceneToViewport`), `:1769` (`PrepareSceneForRender`), `:2077` and `:2085` (`PrepareSceneForRender` and `GpuSceneSyncNode`, inside `ArmGraphViewportFrame` at `:2052`).

A line that is only `GpuSceneInvalidate(reg);` inside `RenderGraph` has zero hits. A line `GpuSceneInvalidate(reg); reg.CreateView<const Transform>();` inside `RenderGraph` has one hit. A line `PickPassIdOf(e); reg.GetComponent<const Transform>(e);` inside `ArmGraphViewportFrame` has one hit. A body whose only tokens are inside `//` and `/* */`, plus a `GpuSceneSyncNode()` call, has zero hits. A body with `GpuSceneSync(reg)` has one hit. Forbidden tokens: `CreateView`, `GetComponent`, `GetResource`, `InspectEntity`, `ActiveSceneCamera`, `ActivePerspectiveSceneCamera`, `CollectPickables`, `PrepareSceneForRender`, `BuildVisibleSet`, `GpuSceneSync`. `CVarRegistry` and `Settings<` are not hits. Reads outside these nine functions stay legal, including the gizmo interaction reads at `:1250`, `:1292`, and `:1348`.

- [ ] **Step 1: Write the failing test.**

`Tests/ArcaneTests/src/RenderRegistryIncludeTest.cpp`. Resolve `cl.exe` inside the test. MSBuild's absolute path does not put `cl` on PATH for a later process whose cwd is the exe dir. The `/showIncludes` half is MSVC-only (spec C15) and sits under `#if defined(_WIN32)` so the Linux and macOS CI legs still compile the file; the host frame-builder scanner below it is portable and runs on every leg. Each module now has its own project file (`Source/<Type>/<Module>/<Module>.vcxproj`, written by `arc_module_project`, `build/modules.lua:391`), so `ShowIncludes` takes the project whose flags compile the TU; ArcaneRender's is the default.

```cpp
#include <catch2/catch_test_macros.hpp>
#include "Helpers/ConstantScan.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstdio>

static std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

#if defined(_WIN32)
static std::filesystem::path ModuleProject(const char* type, const char* module)
{
    return Arcane::Test::RepoRoot() / "Source" / type / module / (std::string(module) + ".vcxproj");
}

static std::string Capture(const std::string& command)
{
    FILE* pipe = _popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string output;
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        output += buf;
    _pclose(pipe);
    return output;
}

struct ClTool
{
    std::string vcvars;
    std::string cl;
};

static ClTool ResolveCl()
{
    const char* pfx86 = std::getenv("ProgramFiles(x86)");
    REQUIRE(pfx86 != nullptr);
    const std::filesystem::path vswhere = std::filesystem::path(pfx86)
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    REQUIRE(std::filesystem::exists(vswhere));
    const std::string vs = Capture(
        "\"" + vswhere.string() + "\" -latest -prerelease -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");
    std::string install = vs;
    while (!install.empty() && (install.back() == '\n' || install.back() == '\r'))
        install.pop_back();
    REQUIRE(!install.empty());
    ClTool tool;
    tool.vcvars = (std::filesystem::path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat").string();
    REQUIRE(std::filesystem::exists(tool.vcvars));
    const std::filesystem::path msvc = std::filesystem::path(install) / "VC" / "Tools" / "MSVC";
    std::string best;
    for (const auto& entry : std::filesystem::directory_iterator(msvc))
        if (entry.is_directory() && entry.path().filename().string() > best)
            best = entry.path().filename().string();
    REQUIRE(!best.empty());
    tool.cl = (msvc / best / "bin" / "Hostx64" / "x64" / "cl.exe").string();
    REQUIRE(std::filesystem::exists(tool.cl));
    return tool;
}

static std::string GrabTag(const std::string& xml, const char* tag)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const std::size_t a = xml.find(open);
    const std::size_t b = xml.find(close, a);
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    return xml.substr(a + open.size(), b - (a + open.size()));
}

static int ShowIncludes(const ClTool& tool, const std::filesystem::path& tu, std::string& output,
                        const std::filesystem::path& vcx = ModuleProject("Client", "ArcaneRender"))
{
    const std::filesystem::path projectDir = vcx.parent_path();
    const std::string xml = ReadAll(vcx);
    const std::string includes = GrabTag(xml, "AdditionalIncludeDirectories");
    const std::string defines = GrabTag(xml, "PreprocessorDefinitions");

    std::string cmd = "cmd /c \"call \"" + tool.vcvars + "\" >nul && \"" + tool.cl
        + "\" /nologo /showIncludes /Zs /std:c++latest /EHsc /MD /Zc:__cplusplus ";
    std::stringstream def(defines);
    std::string item;
    while (std::getline(def, item, ';'))
    {
        if (item.empty() || item.find('%') != std::string::npos)
            continue;
        cmd += "/D" + item + " ";
    }
    std::stringstream inc(includes);
    while (std::getline(inc, item, ';'))
    {
        if (item.empty() || item.find("%(") != std::string::npos)
            continue;   // premake appends %(AdditionalIncludeDirectories)
        const std::string projectToken = "$(ProjectDir)";
        const std::string solutionToken = "$(SolutionDir)";
        if (item.find(projectToken) != std::string::npos)
            item.replace(item.find(projectToken), projectToken.size(), projectDir.string() + "\\");
        if (item.find(solutionToken) != std::string::npos)
            item.replace(item.find(solutionToken), solutionToken.size(),
                         Arcane::Test::RepoRoot().string() + "\\");
        REQUIRE(item.find("$(") == std::string::npos);
        std::filesystem::path path(item);
        if (path.is_relative())
            path = projectDir / path;
        cmd += "/I\"" + path.string() + "\" ";
    }
    cmd += "\"" + tu.string() + "\" 2>&1\"";
    FILE* pipe = _popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe))
        output += buf;
    return _pclose(pipe);
}

static bool RegistryHit(const std::string& output)
{
    return output.find("Astra\\Registry\\Registry.hpp") != std::string::npos
        || output.find("Astra/Registry/Registry.hpp") != std::string::npos;
}

static bool EcsHit(const std::string& output)
{
    return output.find("Arcane\\Ecs.hpp") != std::string::npos
        || output.find("Arcane/Ecs.hpp") != std::string::npos;
}

TEST_CASE("render TUs preprocess Registry.hpp only from ExtractFrame.cpp", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto root = Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render";
    bool sawExtract = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.path().extension() != ".cpp")
            continue;
        std::string output;
        const int code = ShowIncludes(tool, entry.path(), output);
        INFO(entry.path().string());
        INFO(output);
        CHECK(code == 0);
        const bool extract = entry.path().filename() == "ExtractFrame.cpp";
        if (extract)
            sawExtract = true;
        if (extract)
            CHECK(RegistryHit(output));
        else
            CHECK_FALSE(RegistryHit(output));
        if (!extract)
            CHECK(ReadAll(entry.path()).find("Astra::Registry") == std::string::npos);
    }
    CHECK(sawExtract);
}

TEST_CASE("a clean include of DepthConvention passes and SceneCamera fails the same check", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto dir = std::filesystem::temp_directory_path();
    const auto passTu = dir / "f5_depth_only.cpp";
    const auto failTu = dir / "f5_scene_camera.cpp";
    {
        std::ofstream o(passTu);
        o << "#include <Arcane/Scene/DepthConvention.hpp>\n";
    }
    {
        std::ofstream o(failTu);
        o << "#include <Arcane/Scene/SceneCamera.hpp>\n";
    }
    std::string passOut, failOut;
    CHECK(ShowIncludes(tool, passTu, passOut) == 0);
    CHECK_FALSE(RegistryHit(passOut));
    ShowIncludes(tool, failTu, failOut);
    CHECK(RegistryHit(failOut));
    std::filesystem::remove(passTu);
    std::filesystem::remove(failTu);
}

TEST_CASE("cache and debug-draw TUs preprocess neither Registry.hpp nor Ecs.hpp", "[registry-gate]")
{
    const ClTool tool = ResolveCl();
    const auto root = Arcane::Test::RepoRoot() / "Source/Client/ArcaneRender/src/Arcane/Render";
    const char* names[] = {
        "MeshCache.cpp", "MeshMaterialCache.cpp", "SpriteCache.cpp", "PhysicsDebugDraw.cpp" };
    for (const char* name : names)
    {
        std::string output;
        const int code = ShowIncludes(tool, root / name, output);
        INFO(name);
        INFO(output);
        CHECK(code == 0);
        CHECK_FALSE(RegistryHit(output));
        CHECK_FALSE(EcsHit(output));
    }
    // Ruling PR-2: the render node that left the render tree: ArcaneImGui's
    // ImGuiNriNode.cpp, compiled with ArcaneImGui's own flags.
    {
        std::string output;
        const auto node = Arcane::Test::RepoRoot() / "Source/Client/ArcaneImGui/src/Arcane/ImGui/ImGuiNriNode.cpp";
        const int code = ShowIncludes(tool, node, output, ModuleProject("Client", "ArcaneImGui"));
        INFO(output);
        CHECK(code == 0);
        CHECK_FALSE(RegistryHit(output));
        CHECK_FALSE(EcsHit(output));
    }
}
#endif // _WIN32
```

Host gate, same file:

```cpp
// Called on comment-stripped text (HostGateHits), so a brace inside a comment
// cannot end or extend the body; braces inside string and character literals
// are skipped the same way.
static std::string FunctionBody(const std::string& text, const std::string& signature)
{
    const std::size_t sig = text.find(signature);
    if (sig == std::string::npos) return {};
    const std::size_t open = text.find('{', sig);
    if (open == std::string::npos) return {};
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char c = text[i];
        if (quote)
        {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0)
            return text.substr(open, i - open + 1);
    }
    return {};
}

static std::string Strip(std::string row, const char* token)
{
    for (;;)
    {
        const std::size_t at = row.find(token);
        if (at == std::string::npos)
            return row;
        row.erase(at, std::char_traits<char>::length(token));
    }
}

static bool IdentChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
}

static std::string StripComments(const std::string& body)
{
    std::string out;
    out.reserve(body.size());
    enum class Mode { Code, Line, Block, String, Char };
    Mode mode = Mode::Code;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        const char n = (i + 1 < body.size()) ? body[i + 1] : '\0';
        if (mode == Mode::Code)
        {
            if (c == '/' && n == '/')      { mode = Mode::Line;  out.push_back(' '); ++i; }
            else if (c == '/' && n == '*') { mode = Mode::Block; out.push_back(' '); ++i; }
            else if (c == '"')             { mode = Mode::String; out.push_back(c); }
            else if (c == '\'')            { mode = Mode::Char;   out.push_back(c); }
            else out.push_back(c);
        }
        else if (mode == Mode::Line)
        {
            if (c == '\n') { mode = Mode::Code; out.push_back('\n'); }
            else out.push_back(' ');
        }
        else if (mode == Mode::Block)
        {
            if (c == '*' && n == '/') { mode = Mode::Code; out.push_back(' '); ++i; }
            else if (c == '\n') out.push_back('\n');
            else out.push_back(' ');
        }
        else
        {
            out.push_back(c);
            if (c == '\\' && n) { out.push_back(n); ++i; }
            else if ((mode == Mode::String && c == '"') || (mode == Mode::Char && c == '\''))
                mode = Mode::Code;
        }
    }
    return out;
}

static bool WholeWord(const std::string& row, const char* token)
{
    const std::size_t n = std::char_traits<char>::length(token);
    for (std::size_t at = 0; (at = row.find(token, at)) != std::string::npos; at += n)
    {
        const bool left  = at > 0 && IdentChar(row[at - 1]);
        const bool right = at + n < row.size() && IdentChar(row[at + n]);
        if (!left && !right)
            return true;
    }
    return false;
}

static int HostGateHits(const std::string& text, const std::string& signature)
{
    // Strip comments over the whole file first, then find the body: a lone
    // brace in a comment must not truncate or extend the scanned function.
    const std::string body = FunctionBody(StripComments(text), signature);
    if (body.empty()) return -1;
    const char* tokens[] = {
        "CreateView", "GetComponent", "GetResource", "InspectEntity",
        "ActiveSceneCamera", "ActivePerspectiveSceneCamera", "CollectPickables",
        "PrepareSceneForRender", "BuildVisibleSet", "GpuSceneSync"
    };
    const bool stripInvalidate = signature == "RenderGraph("
        || signature == "EditorApp::RenderSceneToViewport(";
    const bool stripPick = signature == "EditorApp::ArmGraphViewportFrame(";
    int hits = 0;
    std::size_t line = 0;
    while (line < body.size())
    {
        const std::size_t end = body.find('\n', line);
        std::string row = body.substr(line, (end == std::string::npos ? body.size() : end) - line);
        if (stripInvalidate) row = Strip(row, "GpuSceneInvalidate");
        if (stripPick) row = Strip(row, "PickPassIdOf");
        for (const char* token : tokens)
            if (WholeWord(row, token))
                ++hits;
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return hits;
}

TEST_CASE("stripping a call name does not exempt the rest of the line", "[registry-gate]")
{
    const std::string mixed =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg); reg.CreateView<const Arcane::Transform>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(mixed, "RenderGraph(") == 1);
    const std::string only =
        "void RenderGraph()\n{\n    GpuSceneInvalidate(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(only, "RenderGraph(") == 0);
    const std::string pick =
        "void EditorApp::ArmGraphViewportFrame()\n{\n    PickPassIdOf(e); reg.GetComponent<Arcane::Transform>(e);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(pick, "EditorApp::ArmGraphViewportFrame(") == 1);
}

TEST_CASE("a brace inside a comment does not end the scanned body", "[registry-gate]")
{
    const std::string brace =
        "void RenderGraph()\n{\n    // } a stray brace\n    reg.CreateView<const Arcane::Transform>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(brace, "RenderGraph(") == 1);
    const std::string literal =
        "void RenderGraph()\n{\n    const char* s = \"}\";\n    reg.GetResource<Arcane::MeshTable>();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(literal, "RenderGraph(") == 1);
}

TEST_CASE("comments and a longer identifier are not host-gate hits", "[registry-gate]")
{
    const std::string comments =
        "void RenderGraph()\n{\n"
        "    // CollectPickables\n"
        "    /* PrepareSceneForRender\n       GpuSceneSync */\n"
        "    GpuSceneSyncNode();\n"
        "}\nvoid Next(){}\n";
    CHECK(HostGateHits(comments, "RenderGraph(") == 0);
    const std::string inString =
        "void RenderGraph()\n{\n    const char* kept = \"// CreateView\";\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(inString, "RenderGraph(") == 1);
    const std::string call =
        "void RenderGraph()\n{\n    GpuSceneSync(reg);\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(call, "RenderGraph(") == 1);
    const std::string onlyNode =
        "void RenderGraph()\n{\n    GpuSceneSyncNode();\n}\nvoid Next(){}\n";
    CHECK(HostGateHits(onlyNode, "RenderGraph(") == 0);
}

TEST_CASE("named frame builders do not read the registry", "[registry-gate]")
{
    const std::string runtime = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp");
    const std::string editor = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorAppFrame.cpp");
    const std::string harvest = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp");
    const std::string preview = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/Documents/ShaderEditorDocument.cpp");
    const std::string editorApp = ReadAll(Arcane::Test::RepoRoot() / "Source/Programs/ArcaneEditor/src/App/EditorApp.cpp");
    CHECK(HostGateHits(runtime, "RenderGraph(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::RefreshSceneResolution(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::RenderSceneToViewport(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::SubmitSceneToBatcher(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::ArmGraphViewportFrame(") == 0);
    CHECK(HostGateHits(harvest, "MaterialPreviewHarvester::Impl::Harvest(") == 0);
    CHECK(HostGateHits(preview, "RenderGraphPreview(") == 0);
    CHECK(HostGateHits(editorApp, "EditorApp::CaptureGraphViewportPng(") == 0);
    CHECK(HostGateHits(editor, "EditorApp::BuildExtractRequest(") == 0);
}
```

- [ ] **Step 2: Build and run the gate.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[registry-gate]"
```

Expected: FAIL. `MeshCache.cpp`, `MeshMaterialCache.cpp`, and `SpriteCache.cpp` each report `Astra/Registry/Registry.hpp` (through `SceneResources.hpp` -> `Ecs.hpp:24`) and `Arcane/Ecs.hpp`. Render's `PhysicsDebugDraw.cpp` and ArcaneImGui's `ImGuiNriNode.cpp` already report neither (rev 5). The comment-token case passes: it only exercises the scanner in this file. The planted `SceneCamera.hpp` TU still reports `Registry.hpp`. The planted mixed `GpuSceneInvalidate` + `CreateView` line reports one hit, the comment body reports zero, and `GpuSceneSyncNode()` reports zero. The cwd of this command is the exe dir; `ResolveCl` is what finds the compiler.

If a `.cpp` outside the table is the failure, stop. The table is the closed set. Do not add an allow-list entry, and do not add a step whose file is chosen by the compiler output. The three cache TUs are in the table. Fixing them is the next step, not a stop.

- [ ] **Step 3: Split the cache records out of `SceneResources.hpp`.**

Do the move, the include retarget, and the three comment updates named in the Interfaces section. `SceneResources.hpp` includes `SceneCacheRecords.hpp`. The three cache headers include `SceneCacheRecords.hpp` and do not include `SceneResources.hpp`. Header-only: no new `.cpp`, and no vcxproj source row. The `/showIncludes` check is what proves the split.

- [ ] **Step 4: Re-run the gate.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[registry-gate]"
```

Expected: PASS, and `.\ArcaneTests.exe "[guard]"` (`ArcaneSpellingGuardTest.cpp`) passes with the new header listed. `ExtractFrame.cpp` is the one `Astra/Registry/Registry.hpp` hit. Each of the five named TUs (the three caches, `PhysicsDebugDraw.cpp`, and `ImGuiNriNode.cpp`) reports neither `Registry.hpp` nor `Ecs.hpp`, and each compile returns 0. The planted `SceneCamera.hpp` TU still reports that path.

- [ ] **Step 5: Run the suite gate, then commit.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5
git add Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneCacheRecords.hpp Source/Runtime/ArcaneScene/src/Arcane/Scene/SceneResources.hpp Source/Client/ArcaneRender/src/Arcane/Render/MeshCache.hpp Source/Client/ArcaneRender/src/Arcane/Render/MeshMaterialCache.hpp Source/Client/ArcaneRender/src/Arcane/Render/SpriteCache.hpp Source/Client/ArcaneRender/src/Arcane/Render/SpriteCache.cpp Source/Client/ArcaneRender/src/Arcane/Render/PhysicsDebugDraw.hpp Tests/ArcaneTests/src/ArcaneSpellingGuardTest.cpp Tests/ArcaneTests/src/RenderRegistryIncludeTest.cpp
git commit -m "test(render): showIncludes and host frame-builder gates" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

`SpriteCache.cpp` is in that list because the comment at `:48` names the header `SpriteEntry` moved out of. `MeshCache.cpp` is not: the comment that names that header is `MeshCache.hpp:39`, already in the list. If a file named in the table still had to change for the gate to pass, add that path too. Do not add an unrelated file. Do not add an allow-list entry.

---

### Task 13: Section 7.1 golden gate

**Files:**
- Modify: only the blessed PNGs the grid/overlay class actually moves, under `ReferenceProject/Verify/References/` (the shared slot) and (ruling PR-3) `ReferenceProject/Verify/References/vulkan-lavapipe/` (the five editor PNGs CI's Mesa lavapipe leg compares against). World lanes are not blessed.

**Interfaces:**
- Consumes: the frame from Tasks 1–12. `scripts/golden-gate.ps1` lanes (`$combos`, `:344-395`): `runtime-scene` and `f3-cull-blend` (backend-split; `--scene 7e5a0030-0030-4030-8030-000000000030` for f3), `editor-ui`, `editor-ui-perspective` (`--view-mode perspective`), `editor-input-doc`, `editor-asset-page`, `editor-material-page`. Every lane runs the host from its exe dir with `--project ReferenceProject --headless --backend <b> --frames 60 --settle 30 --report <path> --compare <ref>` plus the lane's extras (`:972-988`). The gate rebuilds and restages ReferenceProject itself (`:446-500`) but does not rebuild the engine. Comparator success is `diffCount == 0`. The reference tree has three levels: shared `References\<name>.png`, backend `References\vulkan\<name>.png`, and adapter `References\vulkan-lavapipe\<name>.png` (`ReferenceAdapterSet`, `Source/Developer/ArcaneVerify/src/Arcane/Host/ReferenceImages.cpp:36`; picked only on a software adapter).
- Produces: a green Debug gate and a green Release gate, a Dist build, the two local CI legs, a Debug game DLL in the slot, and a commit whose message contains the re-bless note below.

- [ ] **Step 1: Run the Debug gate.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneRuntime\imgui.ini
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneEditor\imgui.ini
Set-Location D:\dev\starworks\Arcane-f5
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: `runtime-scene` and `f3-cull-blend` report `diffCount == 0` on both backends. `editor-ui` and `editor-ui-perspective` report a diff on the grid, and on physics-debug or camera-rect pixels when the frame contains them. Open the diff images the gate writes and confirm the changed pixels are that class. Bless a pixel only when that primitive is present in the new frame and its colour changed because it composites after tonemap. A 2D grid, 3D grid, physics-debug line, or camera rect that is absent — scene or clear colour where that primitive used to draw — is a failure: stop, and leave the PNG blessed as it was. A world sprite or world mesh pixel in the diff stops the task: fix the earlier task, and leave the PNG blessed as it was. A gizmo pixel in the diff stops the task: a display-referred gizmo colour went through ACES. Fix that path and leave the gizmo PNG blessed as it was. An ImGui chrome or widget pixel in the diff stops the task: that class is not the grid. Fix the earlier task and leave the PNG blessed as it was.

`editor-input-doc` and `editor-material-page` cover the viewport with a document tab. They have no pre-authorized bless. If their `diffCount` is 0, leave them. If a pixel moved and the diff image is the same grid/overlay class, it may be blessed with the same note. Any other diff stops the task.

`editor-asset-page` leaves the viewport visible. A grid diff there is the same class. A mesh-thumbnail or HUD diff stops the task.

Do not bless `inprocess-lit-cube`. If `ARCANE_THUMBS` images moved, stop.

- [ ] **Step 2: Re-bless the grid/overlay class from the staged slot, then copy back.**

From the host exe directory so `--project` resolves to the staged tree, with the gate's own lane arguments plus `--bless` (the gate itself never blesses, `golden-gate.ps1:979-983`; the documented bless form is `:138-140`). One dx12 bless covers a shared slot. `editor-ui` and `editor-ui-perspective` are shared (`golden-gate.ps1:347-354`).

```powershell
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneEditor
.\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report "$env:TEMP\f5-bless-editor-ui.json" --compare editor-ui --bless
.\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report "$env:TEMP\f5-bless-editor-ui-perspective.json" --compare editor-ui-perspective --view-mode perspective --bless
```

Bless `editor-asset-page`, `editor-input-doc`, or `editor-material-page` only if Step 1 showed a grid-class diff on that lane, with the lane extras exactly as `$combos` spells them (`golden-gate.ps1:360-375`; the asset is a GUID, not a path):

```powershell
.\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report "$env:TEMP\f5-bless-editor-asset-page.json" --compare editor-asset-page --select-asset d7f389fd-f687-407d-b9d7-9753eb6b0258 --bless
.\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report "$env:TEMP\f5-bless-editor-input-doc.json" --compare editor-input-doc --open-asset 97260310-8b35-4b29-b12f-1fd6f8e99071 --select-in-document Player/Jump --bless
.\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report "$env:TEMP\f5-bless-editor-material-page.json" --compare editor-material-page --open-asset 7e5a0010-0010-4010-8010-000000000010 --bless
```

Immediately copy every PNG this step blessed, and only those, from the staged tree back to source (the next gate run resolves references from source):

```powershell
$src = "D:\dev\starworks\Arcane-f5\ReferenceProject\Verify\References"
$staged = "D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneEditor\ReferenceProject\Verify\References"
$blessed = @('editor-ui', 'editor-ui-perspective')   # add 'editor-asset-page', 'editor-input-doc', 'editor-material-page' only if blessed above
foreach ($name in $blessed) { Copy-Item "$staged\$name.png" "$src\$name.png" -Force }
git -C D:\dev\starworks\Arcane-f5 status --short -- ReferenceProject/Verify/References
```

`git status` lists exactly those PNGs. A bless that wrote a shared lane under `vulkan\` or `vulkan-lavapipe\` is the wrong slot. Shared lanes bless `References\<name>.png` only. Leave that file out of the copy.

- [ ] **Step 3: Re-run Debug, then Release, then Dist, then put the slot back on Debug.**

```powershell
Set-Location D:\dev\starworks\Arcane-f5
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
```

Expected: every lane `diffCount == 0`, both backends.

Release, with no second bless (`build.ps1 Release` rebuilds the game module for Release through `arcbuild build`'s slot check and restages the hosts):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Release
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\Binaries\Release-windows-x86_64-md\ArcaneRuntime\imgui.ini
Remove-Item -ErrorAction SilentlyContinue D:\dev\starworks\Arcane-f5\Binaries\Release-windows-x86_64-md\ArcaneEditor\imgui.ini
Set-Location D:\dev\starworks\Arcane-f5
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Release
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-tests.ps1 Release
```

Expected: PASS with no bless. World lanes still `diffCount == 0`. Editor lanes match the PNGs blessed in Debug. The Release suite passes its baseline check.

Dist, because CI builds it (Global Constraints, Dist and verify guards): `scripts\ci\build.ps1 Dist`, then `scripts\run-tests.ps1 Dist`. Expected: builds, and the suite passes (Dist runs fewer cases by design, `scripts/automation-baselines.json`'s note).

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Dist
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\run-tests.ps1 Dist
```

End on a Debug game DLL, and run the witness that pins `f3-cull-blend` counts against that Debug build:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\build.ps1 Debug
Set-Location D:\dev\starworks\Arcane-f5\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[witness]"
```

`W5` stays `diffCount == 0`. Do not build another configuration after this. The slot `ReferenceProject\Binaries\ReferenceGame.dll` is the Debug CRT.

Then the two local CI legs (windows-msvc Debug natively, linux-gcc-14 Debug in the CI image; `scripts\ci\local.ps1`'s default). They run `~[gpu]` with the per-OS exclusions and must both pass; the Linux leg is the proof that the `#if defined(_WIN32)` guard in Task 12 and every new file compile under GCC:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\local.ps1
```

`local.ps1` builds Windows Debug again, which leaves the slot on Debug.

- [ ] **Step 3b: The lavapipe adapter slot (ruling PR-3).**

CI's linux-gcc-14 Debug leg runs `[gpu]` on Mesa lavapipe (`.github/workflows/ci.yml:209-221`), and its editor witnesses (`EditorWitnessTest.cpp` E2, E3, E4, E5, E6) resolve `ReferenceProject/Verify/References/vulkan-lavapipe/editor-*.png` (five PNGs, last refreshed in `bee29ddcd`). Those PNGs carry the old grid, so the leg goes red on the grid class once this branch reaches CI. Re-bless only those of the five whose diff is the grid/overlay class, only inside the CI image, which carries CI's exact Mesa/LLVM (`ci/linux/Dockerfile:37-53`). No WSL distro, no desk Vulkan, and no other fallback: if any step below cannot run, stop before Step 4 and report (leaving the PNGs or excluding the lanes needs a new ruling; this plan never pushes to let CI produce them). `bee29ddcd` used WSL; that part of its procedure is not reused.

1. Build the branch in the image: `powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-f5\scripts\ci\local.ps1 -Leg linux-gcc-14 -Config Debug`. `container-leg.sh` mirrors the checkout into the named volume `arcane-src-linux-gcc-14` (mounted at `/work`; the checkout itself is mounted read-only at `/src`, `local.ps1:189-195`) and builds `/work/Binaries/Debug-linux-x86_64-md/` (`Binaries/` is outside the rsync mirror, so the build survives in the volume). The leg must pass. A failed build stops the step. A failed test seed may be re-run once with the same command (the build is incremental); a second failure stops the step and is reported.
2. Resolve the image the leg used: `$tag = (& D:\dev\starworks\Arcane-f5\scripts\ci\image-tag.ps1)`; the image is `ghcr.io/starworksdev/arcane-ci:$tag` when it was pulled, or `arcane-ci:$tag-local` when `local.ps1` built it (`local.ps1:90-110`). Set `$image` to that image reference **string** (whichever of the two `docker image inspect` finds), not to `local.ps1`'s internal `$image` object.
3. Compare first, without `--bless`. Each run gets a fresh copy of the staged editor directory, as the witnesses' `WitnessScratch` does (`Tests/ArcaneTests/src/Helpers/HostWitness.cpp:590-605`), so `Saved/` state (thumbnails, recents, the lock) never carries from one lane to the next; `editor-ui` also deletes the layout seed in its copy first, as E6 does (`EditorWitnessTest.cpp:440`). Arguments are the witnesses' own (`EditorWitnessTest.cpp:176-178`, `:238-241`, `:284-287`, `:324-327` for the material document, `:445-447` without `--dump-layout`), with the CI lane's environment (`ci.yml:209-221`; `XDG_RUNTIME_DIR` is a private dir because local containers run as uid 1000). The volume is mounted read-only; everything a run produces is copied to `/out`:

```powershell
$out = "D:\dev\starworks\Arcane-f5\logs\local\lavapipe-bless"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$lanes = [ordered]@{
    'editor-ui'             = '--frames 60 --settle 30'
    'editor-ui-perspective' = '--frames 60 --settle 30 --view-mode perspective'
    'editor-input-doc'      = '--frames 90 --settle 10 --open-asset 97260310-8b35-4b29-b12f-1fd6f8e99071 --select-in-document Player/Jump'
    'editor-asset-page'     = '--frames 60 --settle 30 --select-asset d7f389fd-f687-407d-b9d7-9753eb6b0258'
    'editor-material-page'  = '--frames 90 --settle 10 --open-asset 7e5a0010-0010-4010-8010-000000000010'
}
# Mode: 'compare' (no bless), 'bless', or 'verify' (no bless, against the PNG blessed in step 4).
function Invoke-LavapipeLane([string]$Lane, [string]$Extra, [string]$Mode) {
    $ref  = 'ReferenceProject/Verify/References/vulkan-lavapipe'
    $seed = if ($Lane -eq 'editor-ui') { 'rm -f ReferenceProject/Saved/verify-layout.ini; ' } else { '' }
    $pre  = if ($Mode -eq 'verify') { "if [ -f /out/blessed/$Lane.png ]; then cp /out/blessed/$Lane.png $ref/; fi; " } else { '' }
    $bless = if ($Mode -eq 'bless') { '--bless' } else { '' }
    $post = if ($Mode -eq 'bless') { " && mkdir -p /out/blessed && cp $ref/$Lane.png /out/blessed/" } `
            else { "; cp ReferenceProject/Saved/Verify/$Lane-vulkan-diff.png /out/$Lane-$Mode-diff.png 2>/dev/null; true" }
    $cmd = "rm -rf /tmp/lane && cp -a /work/Binaries/Debug-linux-x86_64-md/ArcaneEditor /tmp/lane && cd /tmp/lane && " +
           "$seed$pre" + "mkdir -p /tmp/xdg && chmod 700 /tmp/xdg && rm -f imgui.ini && " +
           "XDG_RUNTIME_DIR=/tmp/xdg xvfb-run -a -s '-screen 0 1920x1080x24' ./ArcaneEditor --project ReferenceProject " +
           "--headless --backend vulkan --report /out/$Lane-$Mode.json --compare $Lane $Extra $bless$post"
    docker run --rm --init --user 1000:1000 -v arcane-src-linux-gcc-14:/work:ro -v "${out}:/out" `
        -e VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json -e SDL_VIDEODRIVER=x11 `
        -w /tmp $image bash -c $cmd
}
foreach ($lane in $lanes.Keys) { Invoke-LavapipeLane $lane $lanes[$lane] 'compare' }
```

   Each `<lane>-compare.json` under `logs\local\lavapipe-bless\` must name the lavapipe adapter and resolve the `vulkan-lavapipe` level (`ReferenceAdapterSet`, `Source/Developer/ArcaneVerify/src/Arcane/Host/ReferenceImages.cpp:36`); a report that resolved `vulkan/` or the shared slot means lavapipe was not the adapter: stop. Each lane's diff image (`ReferenceProject/Saved/Verify/<lane>-vulkan-diff.png` in that lane's copy, `ReferenceImages.cpp:142-144`) is copied to `logs\local\lavapipe-bless\<lane>-compare-diff.png`; apply Step 1's stop rules to it. A lane with `diffCount == 0` is left alone.
4. Bless only the grid-class lanes: `Invoke-LavapipeLane $lane $lanes[$lane] 'bless'` for each; the blessed PNG lands in `logs\local\lavapipe-bless\blessed\`. Then verify every lane: `foreach ($lane in $lanes.Keys) { Invoke-LavapipeLane $lane $lanes[$lane] 'verify' }` (each fresh copy gets that lane's blessed PNG, when there is one), and require `diffCount == 0` in every `<lane>-verify.json`.
5. Copy the blessed PNGs, and only those, into the checkout:

```powershell
$dst = "D:\dev\starworks\Arcane-f5\ReferenceProject\Verify\References\vulkan-lavapipe"
Get-ChildItem "$out\blessed\*.png" | Copy-Item -Destination $dst -Force
git -C D:\dev\starworks\Arcane-f5 status --short -- ReferenceProject/Verify/References
```

   `git status` must show only those PNGs under `vulkan-lavapipe/`; the shared and `vulkan/` references are unchanged. The volume was never written, and the next `local.ps1` run mirrors the checkout's new PNGs into it.

- [ ] **Step 4: Commit the PNGs and the note.**

The commit message body is the note, verbatim:

```
F5 plan 1: the editor grid, physics-debug lines, and the camera rect leave the linear canvas and composite after the tonemap. A blend into linear colour before the nonlinear tonemap differs from running the tonemap operator on the primitive alone and blending after, including when the post chain is empty. editor-ui and editor-ui-perspective include the boot scene's post process (main.arcscene:27-31). The 2D grid also leaves batcher layer 0, so a grid line that crosses a sprite is in this class. World sprites and world meshes are not in this class. The gizmo was already outside the post chain and its pixels stay. ImGui chrome and widget pixels stay.
```

```powershell
git add ReferenceProject/Verify/References
git commit -m "test(render): F5 plan 1 golden gate, explained grid re-bless" -m "F5 plan 1: the editor grid, physics-debug lines, and the camera rect leave the linear canvas and composite after the tonemap. A blend into linear colour before the nonlinear tonemap differs from running the tonemap operator on the primitive alone and blending after, including when the post chain is empty. editor-ui and editor-ui-perspective include the boot scene's post process (main.arcscene:27-31). The 2D grid also leaves batcher layer 0, so a grid line that crosses a sprite is in this class. World sprites and world meshes are not in this class. The gizmo was already outside the post chain and its pixels stay. ImGui chrome and widget pixels stay." -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi"
```

If a lane did not change, it will not appear in `git status`. Add only the PNGs that changed. Do not add `imgui.ini`.

---

## Spec coverage

| Spec | Task |
|---|---|
| 8.1 declarative clear on `batch2d`, `mesh` loads colour, depth clears to the convention, `CmdClearAttachments` gone for those two planes | 1, 2 |
| 8.1 world-node order unchanged; sprites stay in `batch2d`; meshes stay in the mesh pass | 2, 5, 6, 13 |
| 8.1 overlay after tonemap; 3D grid folds in; the bound packet is what the pass reads; screen UI after the overlay | 3, 9 |
| 8.1 extract + packet; section 4.2 reads move; 4.3 and 4.4 gates | 4, 5, 6, 7, 8, 9, 10, 12 |
| 8.1 `Arcane::Runtime` and ArcaneServer skip extract; `RuntimeApp --headless` runs it | 11 |
| 3.1 depth table, ForwardZ selected, reverse-Z unselected | 1 |
| 3.1 overlay classes, exposure excluded, empty-chain re-bless is not identity | 3, 9, 13 |
| 3.1 chrome graph has no world pass and no overlay | 2 |
| 3.2 G-buffer, lighting, TAA declared and empty. Forward world stays the mesh node. Translucent-world list is plan 3 | 2 |
| 3.3 render origin field exists and is 0; no rebase | 4, 5 |
| 3.4 overlay samples world-pass depth; missing depth draws unoccluded; perspective grid depth reads `SV_Position.z` | 3 |
| 3.6 plans 1–2 keep today's camera split; hosts stop calling the two sweeps | 7, 12 |
| 3.7 server skip; rendering `--headless` runs extract | 11 |
| 4.1 packet fields, `FramesInFlight()` slots, GPU reads the upload ring; the host loop writes `selectedIds` and the outline reads it | 4, 5, 8, 10 |
| 4.3 / 4.4 include gate, cache-header split, and host frame-builder gate | 8, 12 |
| 7.1 world pixels identical; grid class re-blessed with a written note | 13 |
