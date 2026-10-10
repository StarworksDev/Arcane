# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## Repository layout

The repo root IS the premake workspace (`premake5.lua` here generates
`Arcane.slnx` here). This repo was extracted 2026-08-11 from the Aphelyon
monorepo with full filtered history; docs under `docs/` may still cite
pre-extraction paths (`Arcane/...`, `docs/superpowers/...`, `../ThirdParty`).

```
ArcaneCore/        # the shared engine DLL (ArcaneCore.dll): Base/Config/Jobs/
                   #   Material/Mesh/Plugin/Project/Scene/Serialization/Sim/
                   #   Sprite/Assets (the headless engine layer) plus Net/
                   #   Types/Logger + shared header-only utils. Presentation-
                   #   free; every host, tool and game module links it
                   #   (Core-DLL split, 2026-09-15). Namespaced include root
                   #   ArcaneCore/src/Arcane -> #include <Arcane/...>. ZERO
                   #   game references -- keep it that way.
ArcaneClient/      # the engine DLL: Base/Platform/Render/Host (SDL3, NRI on
                   #   D3D12+Vulkan, Batcher2D, ACES tonemap, MSDF text, Assets,
                   #   ImGui, enkiTS jobs, Astra ECS runtime, scene save/load,
                   #   plugin host)
ArcaneRuntime/     # standalone runtime host: opens an .arcproj, runs its game
                   #   module; --frames N = scripted GPU-verify; F5/F6 hot reload
ArcaneEditor/      # the ImGui-on-NRI editor host (namespace Arcane::Editor)
ArcaneHub/         # Tauri launcher (Rust): .arcproj association + recents
ArcaneServer/      # server-side engine tooling (scaffold)
ArcaneTests/       # Catch2 suite + HotReloadPluginV1/V2/Bad fixture DLLs
ReferenceProject/  # in-repo sample game, built like an EXTERNAL project via the
                   #   SDK (build/arcane.lua) -> Binaries/ReferenceGame.dll
ThirdParty/        # vendored deps (see NOTICE.md); premake5 + dxc binaries
build/arcane.lua   # the SDK premake module external games consume (ARCANE_SDK)
data/              # shaders (HLSL sources -> compiled into data/shaders/generated/),
                   #   fonts, EngineConfig
docs/              # specs, plans, audits (engine copy of the monorepo docs)
vcpkg-triplets/    # x64-windows-static-md overlay triplet (v143 toolset) for SDL3
```

## Build

```bat
scripts\setup-vcpkg-deps.bat   # once: SDL3 via the overlay triplet (VCPKG_ROOT required)
GenerateProjects.bat            # premake5 -> Arcane.slnx
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe             # run FROM the exe dir
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --frames 180
```

- Re-run `GenerateProjects.bat` whenever `premake5.lua`, any
  `ThirdParty/*/premake5.lua`, or the file list changes.
- ReferenceProject builds separately (its own workspace over the SDK):
  `cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=Debug /m`.
  Build `ReferenceGame.dll` before the ArcaneRuntime `--frames` GPU-verify.
- Three configurations: Debug / Release / Dist (Dist = no symbols, no
  TRACY_ENABLE).

## Rules baked into the workspace

- **/MD everywhere** (dynamic CRT) -- memory crosses the
  ArcaneClient.dll/Game.dll boundary, so all modules share one heap.
  ThirdParty wrappers are parameterized via `THIRDPARTY_STATICRUNTIME` /
  `THIRDPARTY_PROJECT_LOCATION` (see `ThirdParty/README.md`).
- **No `/fp:fast`** in engine builds (determinism rule). UTF-8 without BOM,
  ASCII comments.
- **Units are MKS** (meters/kg/seconds). Never author pixel-scale content;
  the reference camera maps world->screen at `pixelsPerMeter = 100`.
- **Cvars** live in `ArcaneCore` next to `Config`: typed values (including
  `UInt32`/`UInt64`), names not GUIDs, `SetBy` history, a frame publish barrier,
  default-deny. The editor Console tab and the runtime overlay are the two
  presentations; ImGui is only the drawing. `--set name=value` is the command-line door.
- **Shaders are data:** HLSL sources in `data/shaders/`, compiled by
  `data/shaders/compile-shaders.bat` (DXC; DXIL+SPIR-V; SPIR-V register
  shifts match `nvrhi::VulkanBindingOffsets`: t=0 s=128 b=256 u=384) via the
  ArcaneClient prebuild step into the gitignored `data/shaders/generated/`.
  `ShaderLibrary` loads by name per backend; `ARCANE_SHADER_DIR` overrides
  for the hot-reload dev loop. All scene rendering is linear into the RGBA16F
  Canvas; only TonemapPass (Narkowicz ACES + true sRGB encode) writes the
  backbuffer. `kPxRange`/`kAtlasSize` are mirrored constants between
  `msdf.hlsl` and `TextSystem.cpp`.
- **One TU per module owns the Vulkan-Hpp dynamic dispatcher storage** --
  `ArcaneClient/src/Arcane/Render/VulkanDispatchStorage.cpp`. Projects define
  `NDEBUG` in Release; vulkan.hpp's dispatcher layout is NDEBUG-conditional
  and must match the NRI static lib.
- **ABI bumps are cheap** during engine dev -- never weaken a design to avoid
  a `kGamePluginABIVersion` bump (`ArcaneClient/src/Arcane/Plugin/PluginABI.hpp`).
- The 2D physics engine is **Manifold2D** (github.com/T3mps/Manifold2D),
  vendored at `ThirdParty/Manifold2D` (source-only sync: `include/`+`src/`;
  the vendored `premake5.lua` is THIS repo's consumer wrapper -- never
  overwrite it from the standalone repo). Threading is injected via
  `Arcane/Jobs/ArcaneWorkScheduler.hpp` -> `Manifold2D::IWorkScheduler`.
- **+Y up everywhere (F4, 2026-09-17):** the world is right-handed, +Y up,
  in 2D and 3D alike -- one `ViewTransform` for every consumer, gravity
  defaults to `{0, -9.81}`. Spec: `docs/specs/2026-09-17-f4-editor-3d-authoring-design.md` §2
  (supersedes the physics-wiring spec's "+Y is down"). The editor viewport
  has a 2D | Persp toggle over one editor camera; meshes (and sprites) are
  click-selectable in EVERY view mode (the id pass rasterises world-space
  drawables through the frame's `ViewTransform`, spec §7.1), and the transform
  gizmo is 3D -- one ray-based code path, the 2D view only masks its Z handles
  (planar handles; spec §7.2). It paints as viewport FOREGROUND chrome (an ImGui
  sink over the image, `Viewport/GizmoOverlay.hpp` -- never into the scene
  batch, which the mesh pass overpaints) and its look is Unreal's widget
  (`UnrealWidgetRender.cpp` proportions and colours, in pixels).
- **Bounds, visibility and the GPU scene (F3 plan 1, 2026-09-18, ABI 36, raised to 37 by the review fix 4363b825 -- MeshTable/SpriteTable carry the cache's publish generation):**
  every drawable carries `WorldBounds` (`BoundsSystem`, after transform
  propagation; sprites widen Z only), and each view's `VisibleSet` is the one
  CPU coarse-cull seam sprites, picking, framing and tests share. Meshes draw
  from a persistent GPU instance scene (`GpuSceneMirror` -> `GpuSceneSync` ->
  `GpuScene`, 240-byte rows, dirty-tracked uploads through `gpuscene-sync`)
  through indirect batches, culled on the CPU per view until plan 2's compute
  cull takes over the visible-index loop. Spec:
  `docs/specs/2026-09-18-f3-visibility-and-gpu-scene-design.md`. `Nri/nodes/`
  holds 9 pass types in 7 files (Batch2D, PostChain, Tonemap, Grid, ImGui,
  Mesh, Pick, Outline, GpuSceneSync) against the 10+ domain-reorg trigger --
  plan 2's `MeshCullNode` is the tenth.
- **3D physics is Box3D** (github.com/erincatto/box3d), not Jolt, not a 3D
  Manifold2D. Vendor indefinitely behind a C++ façade; keep a parallel
  engine-owned world. Do not teach `Arcane::PhysicsSystem2D` to write 3D poses (it
  flattens out-of-plane rotation on purpose). Manifold3D is later and
  treats Box3D as the oracle -- Box3D already *is* Rubikon-Lite + Box2D.
  Binding: `docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md`.
- The ECS is **Astra**, vendored at `ThirdParty/Astra` -- keep it current
  with the standalone repo (commit there first, then
  `scripts/sync-vendor.ps1 -Library Astra`; `scripts/sync-astra.ps1` is a
  shim that calls that). Manifold2D and Mosaic use the same script
  (`-Library Manifold2D`, `-Library Mosaic`).
- **3D visual target is Deadlock / Source 2 the renderer, not Unreal.**
  Feature contract: `docs/research/2026-08-12-deadlock-render-target.md`.
  Ceiling, sequencing, and "weeks not department-years": the 2026-09-14
  doc above. **Legal origins / what to implement from:**
  `docs/research/2026-09-14-source2-renderer-public-origins.md` (VRF MIT +
  Valve talks + papers; Source 2 is Valve's own successor, not a third-party
  fork). Nanite/Lumen/scene-SDF-first are non-goals. No leaked Source 2
  source, ever -- including private Rubikon-Lite.
- **Direction and sequencing (2026-09-16, binding):**
  `docs/research/2026-09-16-direction-and-sequencing.md`. Aphelyon (2D) is
  the product; Arcane must not foundationally change under it. Order:
  replication v1 SPEC -> render foundations F4 -> F3 -> F5 -> hygiene wave
  (artifact format stamp, Hub GUID healing) -> cvars -> replication v1
  implementation -> Aphelyon feature work (UI, particles, animation) ->
  Linux/CI -> renderer tiers T1 onward. The 3D renderer tiers are deferred
  SAFELY only because four guards go into the foundation specs: F5 declares
  the DEFERRED pass slots + shared-depth contract (implements forward/2D
  only); F3/F5 reserve TAA jitter + per-object motion vectors; the cooked
  artifact format gets a version stamp before content accumulates; and
  whether Aphelyon needs dynamic 2D lighting is decided BEFORE feature work
  (yes = T1's light grid moves up). Every design still states what Source 2
  does and why we match or diverge.

## CI

Self-hosted Jenkins on the same controller as Aphelyon (`windows && gpu`
interactive agent). The `Jenkinsfile` is the full gate (3 configs, unfiltered
suite including `[gpu]`, golden-gate, `--frames 180`). **The job itself is
not created by checking in this file** -- stand it up once per
`ci/README.md` (Script Console: `ci/create-arcane-job.groovy`). GitHub
Actions (`.github/workflows/ci.yml`) is the hosted `~[gpu]` lane only.

## Tests

- GPU-touching tests are tagged `[gpu]` -- exclude with `~[gpu]` on machines
  without a capable GPU. GPU tests assert `Arcane::RenderErrorCount() == 0`,
  which latches BOTH NRI diagnostics and raw D3D12/VK validation VUIDs --
  validation noise is a test failure.
- ArcaneTests runs in RANDOM order: capture the seed banner on failure and
  reproduce with `--rng-seed N`. Never construct a bare `Arcane::Runtime rt;`
  in a test. Run the suite FROM the exe directory (data paths are
  exe-relative).
- Crashes AND hangs auto-capture symbolized all-thread stacks + a minidump to
  `<exe dir>/diagnostics/` -- read the .txt before theorizing.

## Agent skills and the knowledge graph

Project skills live in `.claude/skills/` (checked in; the rest of `.claude/`
stays ignored). Claude Code loads them on demand by their descriptions; they
carry the repo-specific traps the sections above only summarize:

| Skill | Use it for |
|---|---|
| `arcane-build` | regenerate/build order, the single-slot game DLL, `--project` source-vs-staged resolution, arcbuild |
| `arcane-tests` | running and reproducing ArcaneTests, seeds, which CI lane ran what, the coverage baselines |
| `arcane-verify` | the golden gate, judging lanes, the slot/bless table, copying blesses back to source |
| `arcane-graph` | impact analysis and "what connects to X" via the knowledge graph |

**Use the knowledge graph before assuming how files, symbols or docs
connect.** `graphify-out/` (gitignored; build it per machine with
`/graphify .`, refresh with `/graphify . --update`) graphs every first-party
module plus Astra/Manifold2D/Mosaic together with the specs, plans, research
and audits. Scope is fixed by the root `.graphifyignore`. For impact analysis
run `python .claude/skills/arcane-graph/neighbors.py <Symbol>` AND grep, then
union them -- each finds things the other misses. Confirm in source before
asserting.

**Agent tooling goes the CLI route (shell tools + these skills), not MCP.**
The live-query gap (asking a RUNNING host a new question) is to be closed by
a CLI client to an in-host listener, designed with the cvar/console build.

## Engine-as-SDK

External projects set `ARCANE_SDK` to this repo root and consume
`build/arcane.lua` (see README). The include surface is
`$ARCANE_SDK/ArcaneClient/src` + `$ARCANE_SDK/ArcaneCore/src` + header-only
ThirdParty; the import lib is
`$ARCANE_SDK/bin/<cfg>-windows-x86_64-md/ArcaneClient/ArcaneClient.lib`. The
editor's Build -> Rebuild Game Module spawns `arcbuild.exe` (below); it no
longer resolves premake/msbuild itself (`ArcaneEditor/src/Project/
ModuleBuild.cpp` only composes the `arcbuild` command line and streams its
output).

## arcbuild -- the game-project build driver

`arcbuild.exe` (built to `bin/<cfg>-windows-x86_64-md/arcbuild/`, not on
PATH; a packaged layout ships it beside `ArcaneEditor.exe`, and the editor
looks in both places; specs
`docs/specs/2026-09-13-arcbuild-driver-design.md` +
`docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md`, both
**Implemented**) is the one entry point that drives Premake generation plus a
backend-native build for an SDK-built game project (Aphelyon/Gacha's `Game/`,
this repo's own `ReferenceProject/`, or the `ArcaneTests/data/arcbuild-
fixture/` test fixture) -- the editor, CI, and any script call the same exe
instead of re-implementing premake+msbuild composition three times over.

```bat
arcbuild generate --project <dir|.arcproj> [--action <premake-action>]
arcbuild build    --project <dir|.arcproj> [--config Debug|Release|Dist] [--sdk <root>]
arcbuild rebuild   --project <dir|.arcproj> ...
arcbuild clean     --project <dir|.arcproj> ...
arcbuild probe     --project <dir|.arcproj>    # slot verdict only -- no SDK needed
```

**Backends**, one per Premake action, each with a real resolve/compose/
execute/exit-code contract (no shell -- direct `CreateProcessW` on Windows,
`fork`/`execv` on POSIX):

| Action | Backend | Needs (beyond `ninja`/`make`/`msbuild`/`xcodebuild` itself resolving) |
|---|---|---|
| `vs2026`/`vs2022` (Windows default) | MSBuild | Visual Studio |
| `gmake`/`gmakelegacy` (Linux default) | Make | a GCC/G++ toolchain -- Premake beta8's `gmake` action defaults to GCC on every host, **including Windows** (never `cl.exe`) |
| `ninja` | Ninja | on Windows, a VS developer environment (`cl.exe`/`link.exe`) -- beta8's `ninja` action defaults to MSVC on a native Windows target |
| `xcode4` (macOS default) | xcodebuild | macOS only; `-target <module-stem>`, never `-scheme` (beta8 emits no shared scheme) |

`probe` is the one command that needs neither an SDK nor a backend tool --
it only inspects the manifest and the `Binaries/<gameModule>` slot. Live
Make/Ninja acceptance (real generate/build/rebuild/clean, plus the four
clean-precedence edge cases) is `scripts/verify-arcbuild-backends.ps1` (run
it with `vcvars64` imported; 22 checks); the opt-in `[build-generator]`
Catch2 case characterizes real Premake output for all three non-MSBuild
actions.

**What is live-proven (2026-09-21):** Ninja on Windows is a complete build --
generate/build/rebuild/clean produce and remove `Binaries\Fixture.dll` (the
slot is arcbuild's own staged copy of `Intermediate/<Config>/Ninja/Binaries/`,
because beta8's ninja module cannot run a post-build command on Windows --
see `arcbuild/src/Stage.hpp`). Make on Windows is the full driver mechanics
plus a MinGW-w64 GCC compile of the module; the link then fails because a
GCC object cannot resolve the MSVC-built `ArcaneCore`/`ArcaneClient` import
libraries (C++ mangling) -- a Make-built module needs the GCC-built engine of
the Linux port, not an arcbuild fix. On Linux (the port, 2026-10-05) arcbuild
generates and builds ReferenceProject with the Make backend against the
GCC/Clang-built engine (`Binaries/ReferenceGame.so`), and
`scripts/verify-arcbuild-posix.sh` passes both stages. Xcode's contract is
unit-tested everywhere but **live `xcodebuild` execution needs macOS** --
untested on this all-Windows desk, a standing live-validation limit, not a
gap in the design.
