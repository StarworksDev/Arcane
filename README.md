# Arcane

Arcane is a C++23 game engine. A game is a hot-reloaded shared library
loaded by an editor, a runtime host, or a dedicated server. The renderer
is NRI on D3D12 and Vulkan (MoltenVK on macOS). Gameplay code uses a flat
`Arcane::` API over the Astra ECS and Manifold2D. The editor is ImGui on
the same renderer. Units are metres, kilograms, and seconds. The world is
right-handed, +Y up, and the camera looks down -Z.

This tree is the engine and its SDK. External projects set `ARCANE_SDK` to
the repo root and consume `build/arcane.lua`. The in-repo sample is
[ReferenceProject](ReferenceProject).

## Status

Windows is the desk the editor, the runtime, and the real-GPU image gate
are used on. Linux and macOS build, and they run the test suite in GitHub
Actions. The Linux GPU lane uses Mesa lavapipe (a software Vulkan
device). The macOS GPU lane uses MoltenVK on the runner's Apple
paravirtual device. Interactive use, and play on a real GPU, have not
been hand-verified on Linux or macOS.

| Area | Where it stands |
|---|---|
| Editor, runtime, hot reload, 2D physics, sprites, meshes, cooking | In the tree and used on Windows. |
| Hosted CI on Windows, Linux, and macOS | In the tree. Build and the Catch2 suite run on all three. See [docs/ci.md](docs/ci.md). |
| Real-GPU golden images | Defined in the [Jenkinsfile](Jenkinsfile) for a Windows GPU agent, and run on the Windows desk. |
| Unified 2D and 3D world rendering | In progress on a separate branch. Sprites still draw through the 2D batcher. |
| Entity replication, 3D physics, 64-bit worlds, game UI, VFX, skeletal animation | Specified or planned. Not in this tree. |
| Servitor automation CLI (`servitor`) | Designed. The checked-in note is still a draft, and the tool is not in this tree. |

## Feature overview

### ECS

The ECS is [Astra](ThirdParty/Astra), vendored under `ThirdParty/Astra`.
Game code never includes `Astra::`. [`Arcane/Ecs.hpp`](Source/Runtime/ArcaneScene/src/Arcane/Ecs.hpp)
is the facade: `Arcane::View`, `Arcane::Res`, `Arcane::ResMut`,
`Arcane::Commands`, `Arcane::Entity`, `Arcane::SystemTraits`,
`Arcane::Before`, and `Arcane::After`. `Arcane::Registry` is the Astra
registry.

Entities are 32-bit by default (`ASTRA_ENTITY_BITS`). Systems declare the
components and resources they touch. The scheduler orders them from those
parameters and from `Before` / `After` traits. Phases are
`FixedUpdate`, `Update`, and `Render`. A missing resource skips that
system and logs one line.

`Arcane::Time` is in every world: `realDt`, `dt`, `fixedDt`, `alpha`,
`elapsed`, `fixedStep`, `timeScale`, `paused`, `inFixedStep`.
`Arcane::GameInput` is in client worlds. A server world has none. Code
outside a system, such as a module's `OnUpdate`, reads the same resources
with `Registry().GetResource<Arcane::Time>()`. There is no process-global
world: one process can hold an edit world, a Play world, an embedded
server, and test worlds at the same time.

The flat-API contract is
[docs/superpowers/specs/2026-10-10-arcane-flat-gameplay-api-design.md](docs/superpowers/specs/2026-10-10-arcane-flat-gameplay-api-design.md).

### 2D physics

2D physics is [Manifold2D](ThirdParty/Manifold2D). Game code includes
[`Arcane/Physics2D.hpp`](Source/Runtime/ArcanePhysics2D/src/Arcane/Physics2D.hpp) and talks to
`Arcane::PhysicsWorld2D`, `Arcane::PhysicsSystem2D`, `Arcane::RigidBody2D`,
and `Arcane::Collider2D`.

`RigidBody2D` defaults to `BodyType2D::Kinematic`. Body types are
`Static`, `Kinematic`, and `Dynamic`. Fixture shapes reflected in the
inspector are `Circle`, `Capsule`, and `Aabb`. `Polygon` is a legal value
and is not an inspector option, because a fixture stores no vertex list.

Project gravity defaults to `{0, -9.81}` metres per second squared (+Y up).
A scene can override it with `PhysicsSettings2D`. The parallel solver
exists and stays off unless `physics.parallelSolver` is enabled. The
executor seam is wired: when the solver is on, it runs on the engine job
pool (enkiTS) through Mosaic's `IWorkScheduler`.

Events are pull-model. `StepEvents()` is the latest physics step.
`FrameEvents()` is every step since the frame began. `ContactsOf(entity, out)`
lists what that entity is touching, including sleepers. Each side carries
the entity, a `Guid` (the entity's `Identity` when it has one, otherwise
empty), and a fixture index into `Collider2D`. The entity handle is valid
in the frame it is read. Keep the `Guid` if the identity has to outlive
that frame. Design:
[docs/specs/2026-10-08-physics-2d-events-design.md](docs/specs/2026-10-08-physics-2d-events-design.md).

3D physics is not in the tree. The planned library is Box3D. See the
roadmap.

### Rendering

The device is NRI, with D3D12 and Vulkan backends. macOS uses Vulkan
through MoltenVK. There is no Metal backend. Linux and macOS are
Vulkan-only. The default is 2 frames in flight
(`render.framesInFlight`).

The scene renders linear into an RGBA16F canvas. `TonemapNode` applies a
Narkowicz ACES fit and a true sRGB encode, and it is the pass that writes
the backbuffer. HLSL sources live in `data/shaders/` and compile to DXIL
and SPIR-V (SPIR-V only on macOS). Vulkan register shifts match NRI's
binding offsets. `ARCANE_SHADER_DIR` overrides the shader directory for
the hot-reload dev loop.

What draws today:

- **Batcher2D:** sprites, circles, capsules, and MSDF text. Draws sort by
  a 64-bit key of sorting layer then order in layer. Equal keys have no
  guaranteed order.
- **Meshes:** a persistent GPU instance scene, dirty-tracked uploads, and
  indirect draws. Compute mesh culling runs per view and is on by default.
  Materials are bindless.
- **Other passes:** ground grid, an id pass for picking (sprites and
  meshes in every view mode), selection outlines, a post chain, ImGui,
  and the GPU-scene upload.

World-space sprites still go through Batcher2D. Moving them onto the mesh
path is the unified-world pipeline (F5), which is in progress and not in
this tree. Clustered lighting, shadows, TAA, and global illumination are
roadmap work. The public renderer target is described in
[docs/research/2026-08-12-deadlock-render-target.md](docs/research/2026-08-12-deadlock-render-target.md).

One `ViewTransform` feeds every consumer. A 2D view is an orthographic
camera in metres (`Ortho2DView`). Sprite assets default to 100 pixels per
metre (`SpriteAsset::ppu`). Author content in metres.

### Materials, shaders, and text

The editor has a shader graph and a material graph. Saved materials are
`.arcmat`. Sprites can reference a material; a nil material uses the
plain sprite pipeline. Mesh materials participate in the GPU scene.
MSDF text is a Batcher2D path (`msdf.hlsl` and the text system share
atlas constants). It is the text rasterizer, not a game UI toolkit.

### Editor

`ArcaneEditor` is an ImGui application on the engine renderer. The default
dock layout is a full-height Inspector, an Outliner, a central viewport
with document tabs, and a bottom band: Asset Browser (selected), Asset
Graph, Asset Status, Console, and Problems, plus a second Inspector that
shows assets only.

The viewport toggles 2D and perspective over one editor camera. The
transform gizmo is 3D and draws as viewport chrome (an ImGui overlay on
the image). The 2D view masks its Z handles. Meshes and sprites are
click-selectable in both modes.

Documents include the shader graph, materials, sprites (`.arcsprite`),
meshes (`.arcmesh`), input actions, and crash reports. Play mode can run
standalone, listen-server, embedded-server, or client-only. **Build,
Rebuild Game Module** spawns `arcbuild`. **Create, C++ Class** emits a
component, a system, or a plain class in the flat-API shape.

### Settings and cvars

Cvars are typed values with a `SetBy` history, a frame publish barrier,
and default-deny permissions. Value types are `Bool`, `Int32`, `UInt32`,
`Int64`, `UInt64`, `Float32`, `Float64`, `String`, `Color`, `Vec2`,
`Vec3`, `Vec4`, and `Enum`.

Declare one in a `.cpp` at namespace scope. `.help` is required unless
the cvar is hidden:

```cpp
#include <Arcane/Config/CVarDecl.hpp>

ARC_CVAR(cvar_undoMaxSteps, "editor.undo.maxSteps", std::int32_t, 100,
         .min = 1, .max = 10000, .audience = Arcane::Audience::Editor,
         .scope = Arcane::SettingScope::PreferencesProject,
         .help = "Undo history depth in steps.");
```

`--set name=value` is the command-line door. The editor has Preferences,
Project Settings, and a Console tab. The runtime has an overlay. The
dedicated server reads `get`, `set`, `list`, and `explain` on stdin
through the same remote-cvar service. Config layers and `ARC_SETTINGS`
structs are part of the same system. Design (implemented):
[docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md](docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md).

### Game modules and hot reload

A game module subclasses `Arcane::GameModule` and exports its entry
points with `ARC_GAME_MODULE`. The host checks `kGamePluginABIVersion`
and refuses a mismatch. The current version lives in
[`AbiVersion.hpp`](Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp); its history is in
[`PluginABI.hpp`](Source/Runtime/ArcaneEngine/src/Arcane/Plugin/PluginABI.hpp). Bumping it
is normal during engine development.

```cpp
#include <Arcane/Plugin/GameModule.hpp>

namespace MyGame
{
struct Module final : Arcane::GameModule
{
    void OnDrawUI() override {}
};
}

ARC_GAME_MODULE(MyGame::Module)
```

Hooks include `OnInit`, `OnShutdown`, `OnFixedUpdate`, `OnUpdate`,
`OnDrawUI`, and save/load state for hot reload. The engine installs its
own systems (`PhysicsSystem2D`, transform propagation, render
submission). A module registers only its own, with `ARC_SYSTEM` or
`RegisterSystem` in `OnInit`.

The plugin host watches the module file and reloads it (debounced, state
kept). In the runtime, F5 is `reload_plugin` (state kept) and F6 is
`reload_plugin_fresh`. Hot reload is refused while a net driver reports
itself active. On macOS a module unloads only if it holds no thread-local
variables, so game modules build with spdlog's thread-local storage off
(`SPDLOG_NO_TLS`); a `thread_local` in your own game code keeps the module
loaded and blocks hot reload there. Secondary plugins load once and do not hot-reload on
their own.

The manifest may name the module `MyGame.dll`. Off Windows the loader
maps that stem to `MyGame.so` or `MyGame.dylib`.

### Assets and cooking

`arccook` cooks textures (BC7) and meshes (cgltf and meshoptimizer) into
`.arcart` artifacts. The common prefix carries an artifact version,
content kind, source guid, source hash, and importer version. Content
kinds today are texture and mesh. The client reads artifacts with its
own reader. It does not link `ArcaneAssetPipeline` (that static library
is the cook side, used by `arccook` and the editor cook queue).

```bat
Binaries\Debug-windows-x86_64-md\arccook\arccook.exe --project ReferenceProject --check
```

`--check` exits 0 when nothing is stale and 2 when something is.
`--dump-dds` cooks, then writes a DDS for one texture guid.

Scenes are JSON (`.arcscene`), `kSceneJsonVersion` 6, and files back to
version 3 still load. Projects are `.arcproj`. Sprites are `.arcsprite`.
Materials are `.arcmat`. Meshes in the editor are `.arcmesh`.

### Networking today

`NetMode` is `Standalone`, `DedicatedServer`, `ListenServer`, or
`Client`. `RoleMask` is `Server`, `Client`, or `Both`. Play topologies
are `Standalone`, `ListenServer`, `EmbeddedServer`, and `ClientOnly`.
They choose which systems run. They do not replicate entities.

`INetDriver` is the transport seam (`IsActive()`). No production driver
ships. A length-framed JSON protocol over TCP
([`Net/Protocol.hpp`](Source/Runtime/ArcaneNet/src/Arcane/Net/Protocol.hpp)) is what
the protocol fuzzer exercises.

`ArcaneServer` is a dedicated-server host. It links `ArcaneCore` and not
`ArcaneClient`. It opens an `.arcproj`, loads the game module, and ticks
a fixed step. It does not replicate.

Replication with snapshots, prediction, and relevance is specified and
not implemented. See the roadmap.

### Diagnostics

A crash or a hang writes a text report and a minidump under
`<exe dir>/diagnostics/`. Tests and hosts should be diagnosed from that
text before theorizing.

On Windows, `ArcaneCrashReporter` is a separate process: a window, a
symbolizer, and a hang monitor launched beside the host.

On Linux and macOS the same reports are written in-process. Fatal
signals run on an alternate stack. Stacks are frame-pointer walks. The
dump is Breakpad format. macOS reads suspended threads through Mach.
There is no `ArcaneCrashReporter` on those platforms, so the hand-off to
a reporter process reports failure, and `launchMonitor` is ignored.
The hang witness (H1) and the reporter/monitor cases stay excluded there
for that reason. GPU fault breadcrumbs are a Windows D3D12 path.

### Audio

`Arcane::Audio::AudioDevice` plays sounds through miniaudio: buses,
voices, volume, pitch, and pan. Mutators run on the main thread. Hosts
can force the null backend (the usual choice for `--frames` and CI).
A mixer graph, spatialization, streaming cues, and an audio editor are
planned and not built.

## Platforms and requirements

| | Windows | Linux | macOS |
|---|---|---|---|
| CI host | `windows-2025`, MSVC (VS 2026) | `ubuntu-24.04`, GCC 14 and Clang 19 on libstdc++ 14 | `macos-15`, Apple silicon, Xcode 16.4, Apple Clang, libc++ |
| Graphics in CI | The CPU suite. Real GPU is the Jenkins agent. | `[gpu]` on lavapipe, GCC 14 Debug only | `[gpu]` on MoltenVK, Debug only |
| Renderer you can select | D3D12 and Vulkan | Vulkan | Vulkan (MoltenVK) |
| Default `arcbuild` action | `vs2026` | `gmake` | `gmake` (`xcode4` remains available) |
| Output directory | `Binaries\Debug-windows-x86_64-md` | `Binaries/Debug-linux-x86_64-md` | `Binaries/Debug-macosx-AARCH64-md` |

Toolchains below the CI pins do not build the engine. GCC 13 lacks the
C++23 feature set the code uses. Clang needs libstdc++ from GCC 14.
Intel Macs are not tested. `ARCANE_MAC_ARCH=arm64|x86_64` exists for a
Mac target, and the fetched premake binary for macOS is arm64.

Three configurations: **Debug**, **Release**, and **Dist**. Dist ships
without symbols. Every module uses the dynamic CRT (`/MD` on Windows) so
the engine and the game module share one heap. Engine builds do not use
fast-math.

SDL3 comes from vcpkg on Windows. Linux and macOS build it from source
with [`scripts/build-sdl3.sh`](scripts/build-sdl3.sh) (SDL 3.2.22, the
version the script pins). Neither Ubuntu 24.04 nor a stock Mac packages
it.

The macOS port notes, including MoltenVK gaps measured on the runner,
are [docs/research/2026-10-07-macos-moltenvk-port.md](docs/research/2026-10-07-macos-moltenvk-port.md).
Apple libc++ has no IANA time-zone database. Crash-report timestamps
fall back when a named zone is unavailable.

## Repository layout

The repo root is the Premake workspace. `GenerateProjects.bat` writes
`Arcane.slnx` here. `ReferenceProject` is a second workspace that
consumes the SDK the way an external game does. `ArcaneHub` is not in
the C++ solution.

| Path | What it is |
|---|---|
| `Source/Runtime/` | Runtime module DLLs, every host (servers included): `ArcaneCore` (Base, Config, Jobs, Platform, Cli, reflection), `ArcaneNet`, `ArcaneCrypto`, `ArcaneAssets`, `ArcaneScene`, `ArcanePhysics2D`, `ArcaneSim`, `ArcaneEngine` (runtime, plugin host, projects). Include root `#include <Arcane/...>`. No game references. |
| `Source/Client/` | Client module DLLs, windowed hosts: `ArcaneWindow`, `ArcaneInput`, `ArcaneAudio`, `ArcaneRender` (NRI, the render graph, Batcher2D), `ArcaneImGui`, `ArcaneClient`. |
| `Source/Developer/`, `Source/Editor/` | `ArcaneAssetPipeline` (cook importers), `ArcaneVerify` (the verify harness), `ArcaneEdit`. |
| `Source/Programs/` | `arccook` (command-line cooker), `arcbuild` (game-project build driver: Premake, then MSBuild, Make, Ninja, or xcodebuild), `ArcaneRuntime` (windowed runtime host), `ArcaneEditor` (editor host), `ArcaneServer` (dedicated server host, Runtime modules only), `ArcaneCrashReporter` (Windows-only crash and hang reporter). |
| `Tests/` | `ArcaneTests` (Catch2 suite, its data and reference images) and `Fixtures/` (hot-reload modules, process/death fixtures, game-module fixtures). |
| `ArcaneHub/` | Tauri and Svelte launcher. Recents, and on Windows the `.arcproj` file association. |
| `ReferenceProject/` | Sample game. Builds to `Binaries/ReferenceGame.dll` (`.so` / `.dylib` off Windows). |
| `ThirdParty/` | Vendored dependencies. See [NOTICE.md](NOTICE.md). |
| `build/arcane.lua` | SDK Premake module. |
| `data/` | Shader sources, fonts, engine input config. Generated shaders are gitignored. |
| `docs/` | Specs, research, and [docs/ci.md](docs/ci.md). |
| `scripts/` | Setup, test runners, the golden gate, fuzz, tool fetches. |
| `fuzz/` | libFuzzer harnesses. See [docs/fuzzing.md](docs/fuzzing.md). |

Off Windows the engine libraries are `libArcaneCore.so` /
`libArcaneClient.so` and `libArcaneCore.dylib` /
`libArcaneClient.dylib`. Executables have no `.exe` suffix.
`scripts/arcane-bin-dir.sh Debug` prints the bin directory for a
configuration.

## Building

Authoritative detail, pins, and the CI matrix:
[docs/ci.md](docs/ci.md). Premake is 5.0.0-beta8.
[`scripts/fetch-premake.sh`](scripts/fetch-premake.sh) downloads the
pinned binary. A copy also ships at `ThirdParty/premake5/` for Windows.

Re-run Premake when `premake5.lua`, a vendored `premake5.lua`, or a
file list changes. Build the sample module before a host that loads it.
The hosts stage `ReferenceProject` beside themselves.

### Windows

Visual Studio 2026, "Desktop development with C++", and
[vcpkg](https://vcpkg.io) with `VCPKG_ROOT` set.

```bat
scripts\setup-vcpkg-deps.bat
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
```

`setup-vcpkg-deps.bat` installs SDL3 with the overlay triplet in
`vcpkg-triplets/` (dynamic CRT; SDL3 itself as `SDL3.dll`, staged beside
every program -- `ThirdParty/SDL3/SDL3.arcmodule`).

CPU tests, from the exe directory, two equivalent doors:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-tests.ps1 Debug --rng-seed 1
```

```bat
cd Binaries\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu] --rng-seed 1
```

`scripts\run-tests.ps1` always applies `~[gpu]`. It has no GPU switch.
The unfiltered suite, including `[gpu]`, is what the Jenkins job runs.
You can also run `ArcaneTests.exe` with no tag filter from that
directory.

The sample, as its own workspace (`ARCANE_SDK` is optional here; the
Premake file locates the engine one directory up):

```bat
cd ReferenceProject
..\ThirdParty\premake5\premake5.exe vs2026
msbuild ReferenceProject.slnx /p:Configuration=Debug /m
```

### Linux

GCC 14 or Clang 19, plus the pinned SDL3 prefix. The snippet uses GCC,
matching the `linux-gcc-14` CI leg. Clang is the same commands with
`CC=clang-19`, `CXX=clang++-19`, and `--cc=clang`.

```bash
scripts/fetch-premake.sh
scripts/fetch-dxc-linux.sh
scripts/build-sdl3.sh "$HOME/sdl3-prefix"
export SDL3_ROOT="$HOME/sdl3-prefix"
export CC=gcc-14 CXX=g++-14
ThirdParty/premake5/premake5 --cc=gcc gmake
make -j3 config=debug ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient ArcanePhysics2D arcane-abi-digest
"$(scripts/arcane-bin-dir.sh Debug)/arcbuild/arcbuild" build --project ReferenceProject --config Debug --sdk "$PWD"
make -j3 config=debug ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
scripts/run-tests.sh Debug --rng-seed 1
```

Keep `-j` low. Linking Debug `ArcaneTests` peaks around 500 MB per
link, and several links at once will thrash a small machine. CI uses
`-j3`.

The game module is built before the hosts because their post-build step
stages `ReferenceProject`. `config=debug`, `config=release`, and
`config=dist` are the three gmake configurations.

`scripts/run-tests.sh` runs `~[gpu]` unless you pass `--gpu`. With
`SDL_VIDEODRIVER` unset it uses SDL's offscreen driver. Cases that
cannot pass on Linux are listed, each with a reason, in
[`scripts/linux-test-exclusions.txt`](scripts/linux-test-exclusions.txt).

The GPU lane (lavapipe plus the pinned Vulkan validation layers):

```bash
# Build the pinned layers into a user-owned prefix, then install them into
# /usr/local (the loader search root the script expects), as CI does.
# Needs git, cmake, ninja and a C++ toolchain.
scripts/build-vvl-linux.sh "$HOME/vvl-prefix"
sudo cp -a "$HOME/vvl-prefix/." /usr/local/
sudo ldconfig
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export SDL_VIDEODRIVER=x11
xvfb-run -a -s "-screen 0 1920x1080x24" scripts/run-tests.sh --gpu Debug --rng-seed 1
```

Install those validation layers only through
`scripts/build-vvl-linux.sh`. Leave Ubuntu's
`vulkan-validationlayers` package off that machine: it is an older
layer set, and the suite expects the pin in the script.
[`scripts/linux-windowed-smoke.sh`](scripts/linux-windowed-smoke.sh)
opens a real window under Xvfb on both X11 and Wayland. CI runs that
smoke on the GCC 14 Debug leg.

Shader compilation on Linux is `data/shaders/compile-shaders.sh`
(DXIL and SPIR-V), invoked by the Premake prebuild step.
`scripts/fetch-dxc-linux.sh` fetches the compiler.

### macOS

Apple silicon, Xcode 16.4, and the same SDL3 build. CI selects that
Xcode on purpose:

```bash
sudo xcode-select -s /Applications/Xcode_16.4.app
scripts/fetch-premake.sh
scripts/fetch-vulkan-sdk-macos.sh --system
scripts/build-sdl3.sh "$HOME/sdl3-prefix"
export SDL3_ROOT="$HOME/sdl3-prefix"
ThirdParty/premake5/premake5 --cc=clang gmake
make -j3 config=debug ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient ArcanePhysics2D arcane-abi-digest
"$(scripts/arcane-bin-dir.sh Debug)/arcbuild/arcbuild" build --project ReferenceProject --config Debug --sdk "$PWD"
make -j3 config=debug ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
scripts/run-tests.sh Debug --rng-seed 1
```

`scripts/fetch-vulkan-sdk-macos.sh --system` installs MoltenVK, the
Vulkan loader, validation layers, and DXC under `/usr/local`. The GPU
lane:

```bash
export VK_DRIVER_FILES=/usr/local/share/vulkan/icd.d/MoltenVK_icd.json
export SDL_VIDEODRIVER=cocoa
scripts/run-tests.sh --gpu Debug --rng-seed 1
```

Exclusions:
[`scripts/macos-test-exclusions.txt`](scripts/macos-test-exclusions.txt).
Editor golden witnesses E2 through E6 are excluded on this lane because
text and edges rasterize differently from the Windows references, and
that device has no blessed golden slot. The runtime golden witnesses
W1 through W6, and `f3-cull-blend`, match. Shaders compile to SPIR-V
only (`data/shaders/compile-shaders.sh`).

A desk Mac and a real discrete GPU have not been part of the
verification. The results above are the GitHub `macos-15` runner.

## Running

Paths below are the Windows Debug layout. On Linux and macOS drop
`.exe` and use the bin directory from the table above (or
`scripts/arcane-bin-dir.sh`).

### Editor

```bat
Binaries\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject
```

Headless captures used by the golden gate add `--headless`, `--backend`
(`dx12` or `vulkan`), `--frames`, `--settle`, `--report`, and
`--compare`. `--settle` is refused unless `--report` or `--screenshot`
is also set.

### Runtime

```bat
Binaries\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject
```

`--frames N` runs N frames and exits. That is the scripted GPU check.
F5 reloads the game module and keeps state. F6 reloads it fresh
(engine input config included). `--scene <guid>` opens a scene other
than the project boot scene.

### Server

```bat
Binaries\Debug-windows-x86_64-md\ArcaneServer\ArcaneServer.exe --project ReferenceProject --frames 300 --report server.json
```

The process ticks until `--frames` completes, or until Ctrl-C when
`--frames` is 0. Stdin is an admin console (`get`, `set`, `list`,
`explain`, `help`) unless `--no-admin-console` is set.
`--report` is written when the loop ends. A server started with
`--frames 0` and then killed does not write that file.
`--print-engine-info` prints identity JSON and exits.

### Hub

`ArcaneHub` is the launcher: recent projects, and on Windows the
`.arcproj` association. It is a Tauri application (Rust and Svelte),
built with Node and Cargo, not with `Arcane.slnx`. The installer
workflow is
[`.github/workflows/build-arcane-hub.yml`](.github/workflows/build-arcane-hub.yml)
(Windows, NSIS). No Hub binary is committed.

## Making a game

### Consume the SDK

Point `ARCANE_SDK` at this repo. A game workspace is a Premake file next
to the `.arcproj`:

```lua
workspace "MyGame"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }

include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
arcane_game_module("MyGame")
```

`arcane_game_module` reads `sourceDir` from that `.arcproj` (default
`Source/`) and declares a C++23 shared library into `Binaries/`. On
macOS the helper selects the host architecture, so the workspace can
keep `architecture "x64"`. The built file is `MyGame.dll`, `MyGame.so`,
or `MyGame.dylib`. The manifest may still say `MyGame.dll`.

`ReferenceProject/premake5.lua` is the worked example. Inside this repo
it locates the SDK itself when `ARCANE_SDK` is unset.

### Declare the engine modules the game uses

`<sourceDir>/MyGame.arcmodule` names the engine modules the game links.
The SDK links and includes their closure and nothing else, so including
a header from an undeclared module is a missing-header error.
`ReferenceProject/Source/Game/ReferenceGame.arcmodule`:

```json
{
  "name": "ReferenceGame",
  "type": "Game",
  "dependencies": {
    "public": [],
    "private": ["ArcaneClient", "ArcaneCore", "ArcaneEngine", "ArcaneInput",
                "ArcanePhysics2D", "ArcaneScene", "ArcaneSim"]
  },
  "thirdParty": { "public": [], "private": [] }
}
```

`name` must equal the file stem and the `arcane_game_module` name. A game
may depend on Runtime and Client modules only. `ImGui` and `ArcaneImGui`
are modules too: list `ArcaneImGui` to draw from `OnDrawUI` (the macro
adopts the host's ImGui context only when ImGui is in the closure).
Without a descriptor a game gets every Runtime and Client module plus
`ImGui` and `ArcaneImGui`. A new project and the editor's
Create -> C++ Class write that default descriptor when none exists.

A non-game consumer, such as a server executable, calls
`arcane_module_consumer { "ArcaneCore", "ArcaneNet" }` inside its own
`project` block and `arcane_stage_modules { ... }` to copy the module
DLLs beside an executable.

### Package a game as one executable

`arcbuild package` links the engine, the standalone host and the game
into one `<Project>.exe` and stages it with what it needs to run:

```bat
Binaries\Debug-windows-x86_64-md\arcbuild\arcbuild.exe package --project MyGame --config Release --sdk %ARCANE_SDK%
```

It builds the engine's monolithic layout (`premake5 --layout=monolithic`:
every Runtime and Client module as a static library, in
`Binaries/<cfg>-<sys>-<arch>-mono/`), regenerates the project with the same
option (the game becomes a static library plus a `<Project>Package`
executable, generated under `Intermediate/Package/Workspace/`), builds
it, and stages `Packages/<Project>-<platform>-<config>/` in Unreal's layout:

```
<Project>.exe | <Project>.sh      launcher (Windows stub / Linux+macOS script)
<Project>/
  Binaries/<Win64|Linux|Mac>/     the game, SDL3, DXC, the Agility SDK, engine data/
  <Project>.arcproj  Content/  Config/  Intermediate/Artifacts/
```

The launcher runs `<Project>/Binaries/<Platform>/<Project>` with the same
arguments and returns its exit code; on Linux and macOS the game finds its
shared libraries beside itself (`$ORIGIN` / `@executable_path`). Run it in
place; it opens the staged project when no `--project` is given. A package never
contains a Developer or Editor module, so the verification flags
(`--headless`, `--report`, `--compare`) belong to `ArcaneRuntime.exe`.

### A module, a component, a system

Game code spells `Arcane::` names. The samples below match
`ReferenceProject/Source/Game` and the headers they include.

A component is reflected plain data. `ARC_REFLECT_ATTR` looks attributes
up in `Arcane::Attr` (`Range` is one of them). Register it with one
`ARC_COMPONENT` line in a `.cpp`:

```cpp
#include <Arcane/Reflection.hpp>

struct Health { float current = 100.0f; };

ARC_REFLECT_TYPE(Health)
    ARC_REFLECT_FIELD(Health, current)
        ARC_REFLECT_ATTR(Range, 0.0f, 100.0f)
ARC_END_REFLECT_TYPE()
```

```cpp
#include "Health.hpp"
#include <Arcane/Plugin/GameComponents.hpp>

ARC_COMPONENT(Health)
```

A system lists the views and resources it uses. This one runs on the
client, in the fixed step, before `PhysicsSystem2D`. `ActionRef` is
default-constructible, which `ARC_SYSTEM` requires. `Time` is available
the same way (`Arcane::Res<Arcane::Time>`) when the system needs
`fixedDt` or `alpha`.

```cpp
#include <Arcane/Ecs.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Physics2D.hpp>

namespace MyGame
{
struct Jumper : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem2D>>
{
    Arcane::ActionRef jump{"Player", "Jump"};

    void operator()(Arcane::View<Arcane::RigidBody2D>& view,
                    Arcane::Res<Arcane::GameInput> input,
                    Arcane::ResMut<Arcane::PhysicsWorld2D> physics)
    {
        if (!input->PressedThisFixedStep(jump))
            return;
        view.ForEach([&](Arcane::Entity e, Arcane::RigidBody2D& body)
        {
            const Arcane::BodyMotion2D m = physics->Motion(e, body);
            if (m.supported)
                physics->SetVelocity(e, body, m.velocityX, 6.0f);
        });
    }
};
}
```

```cpp
#include "Jumper.hpp"
#include <Arcane/Plugin/GameSystems.hpp>

ARC_SYSTEM(MyGame::Jumper, Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate)
```

`GameInput` methods used by gameplay include `Value`, `Down`, `Pressed`,
`Released`, `PressedThisFixedStep`, and `ReleasedThisFixedStep`. The
reference controller
([`PlayerController2DSystem.hpp`](ReferenceProject/Source/Game/PlayerController2DSystem.hpp))
is the full version of this pattern: move, jump buffer, coyote time.

Physics events are the same resource. This follows
[`TintOnContactSystem.hpp`](ReferenceProject/Source/Game/TintOnContactSystem.hpp):

```cpp
#include <Arcane/Ecs.hpp>
#include <Arcane/Physics2D.hpp>
#include <Arcane/Scene/Components.hpp>

#include <glm/vec4.hpp>

struct TintOnHit
{
    void operator()(Arcane::View<Arcane::SpriteRenderer>& view,
                    Arcane::Res<Arcane::PhysicsWorld2D> physics)
    {
        const Arcane::PhysicsEvents2D events = physics->FrameEvents();
        view.ForEach([&](Arcane::Entity entity, Arcane::SpriteRenderer& sprite)
        {
            for (const Arcane::ContactBegin2D& hit : events.contactBegin)
            {
                if (hit.a.entity == entity || hit.b.entity == entity)
                {
                    sprite.tint = glm::vec4{1.0f, 0.25f, 0.25f, 1.0f};
                    return;
                }
            }
        });
    }
};
```

`sprite.tint` is a linear `glm::vec4`. Register `TintOnHit` with
`ARC_SYSTEM` in the `Update` phase: `FrameEvents()` gathers every fixed
step of the frame.

### arcbuild

`arcbuild` is the entry point the editor, CI, and scripts use to
generate and build a game module. Specs:
[docs/specs/2026-09-13-arcbuild-driver-design.md](docs/specs/2026-09-13-arcbuild-driver-design.md),
[docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md](docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md).

```bat
Binaries\Debug-windows-x86_64-md\arcbuild\arcbuild.exe generate --project MyGame
Binaries\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build    --project MyGame --config Debug --sdk %ARCANE_SDK%
Binaries\Debug-windows-x86_64-md\arcbuild\arcbuild.exe probe    --project MyGame
```

Commands are `generate`, `build`, `rebuild`, `clean`, `probe`, and
`package` (the one-executable build above; MSBuild and Make actions, so
set `MAKEFLAGS=-jN` for a parallel engine build).
`probe` only inspects the manifest and the `Binaries/` slot. It needs
no SDK. Other flags: `--action`, `--force-rebuild`, `--quiet`, `--set`.

| `--action` | Backend | Notes |
|---|---|---|
| `vs2026`, `vs2022` | MSBuild | Windows default is `vs2026`. |
| `gmake`, `gmakelegacy` | Make | Linux and macOS default. Premake's `gmake` action uses GCC on every host, including Windows. A GCC object cannot link the MSVC-built engine import libraries, so a Make-built module on Windows needs a GCC-built engine. |
| `ninja` | Ninja | On Windows, a VS developer environment. `arcbuild` copies the linked module into `Binaries/` itself, because this Premake version cannot run a post-build step on the Windows Ninja generator. |
| `xcode4` | xcodebuild | Available on macOS (`-target`, the module stem). The hosted macOS job uses `gmake`. |

The process is spawned directly (`CreateProcessW` on Windows,
`fork`/`exec` on POSIX). There is no shell in the middle.

## Testing and verification

### Catch2

`ArcaneTests` is the unit and integration suite. Run it from its own
directory. Data paths are relative to that directory, and the suite
order is random. The banner prints the seed. Reproduce a failure with
`--rng-seed N`.

`~[gpu]` drops device cases. It keeps `[golden]` and `[mesh]` cases
that are not tagged `[gpu]`. Those are part of the CPU baseline. GPU
cases assert `Arcane::RenderErrorCount() == 0`, which latches NRI
diagnostics and raw D3D12 and Vulkan validation messages. A validation
message fails the test.

Hosted CI runs the CPU filter twice per leg, on
`github.run_number` and on `(run_number + 50021) mod 4294967296`.

### Golden gate

[`scripts/golden-gate.ps1`](scripts/golden-gate.ps1) launches the real
runtime and editor, headless, against `ReferenceProject`, and compares
each capture to a blessed PNG. It is fourteen lanes: seven references,
each on D3D12 and Vulkan. The [Jenkinsfile](Jenkinsfile) defines it for a
Windows GPU agent, and it runs on the Windows desk. Hosted GitHub Actions
does not run it.

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
type Binaries\Debug-windows-x86_64-md\golden-gate-summary.json
```

Assert `gatePassed` and each lane's `verdict`. Ignore the process exit
code. The summary is schema version 3. If the gate refuses to run (the
sample failed to build, `automation-exclusions.json` is malformed, or
`-SelfTest` sees a dirty tree), the file is still rewritten:
`gatePassed` false, `refusalReason` set, and no lanes. A missing file
is not a green result. The script deletes the summary at start.

| Reference | Host | Level | Extra arguments | Self-test |
|---|---|---|---|---|
| `runtime-scene` | Runtime | backend | | Failed |
| `editor-ui` | Editor | shared | | Failed |
| `editor-ui-perspective` | Editor | shared | `--view-mode perspective` | Failed |
| `editor-input-doc` | Editor | shared | `--open-asset 97260310-8b35-4b29-b12f-1fd6f8e99071 --select-in-document Player/Jump` | Green |
| `editor-asset-page` | Editor | shared | `--select-asset d7f389fd-f687-407d-b9d7-9753eb6b0258` | Failed |
| `editor-material-page` | Editor | shared | `--open-asset 7e5a0010-0010-4010-8010-000000000010` | Green |
| `f3-cull-blend` | Runtime | backend | `--scene 7e5a0030-0030-4030-8030-000000000030` | Green |

`runtime-scene` and `f3-cull-blend` are backend-split: Vulkan has its
own PNG, and D3D12 uses the shared file and reports `PassedOnFallback`.
The five editor references are shared. Bless a backend-split reference
on each backend. Bless a shared reference once.

`-SelfTest` mutates the sample's boot scene and expects the verdict in
the table. Lanes that show that scene go red. Lanes on another scene,
or a document that covers the viewport, stay green. Restoration is in
`try/finally`. The self-test writes
`golden-gate-selftest-summary.json` beside the normal summary, so it
does not replace a green `gatePassed` artifact. Jenkins runs the
self-test on `main` and `milestone/*` only, after the ordinary gate.
The gate itself runs Release, then Debug. Debug has to finish second:
the next Jenkins stage builds Debug into the single
`ReferenceProject/Binaries` slot.

When the device is a software adapter, compare looks in an adapter
directory first (`vulkan-lavapipe`, `dx12-warp`, or
`<backend>-software`), then `References/<backend>/`, then the shared
file. A bless on a named software adapter writes that adapter
directory. Hardware devices get no adapter directory. MoltenVK on the
Apple paravirtual device is treated as hardware, which is why the
macOS editor goldens are exclusions rather than a new slot.

### Blessing a reference

`--bless` writes the capture to the level compare resolved. `--project`
is relative to the working directory. From the repo root the write
lands in source `ReferenceProject/Verify/References`. From the host's
exe directory (how the gate runs) it lands in the staged copy under
`Binaries/<config>/<Host>/ReferenceProject`, and the next sample build
overwrites that copy. Bless from the repo root, or copy the PNG back
into source. `git status` should show the reference.

```bat
Binaries\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject ^
  --headless --backend dx12 --frames 60 --settle 30 --report r.json --compare runtime-scene --bless
```

`--report` or `--screenshot` is required with `--settle`.
`editor-ui-perspective` needs `--view-mode perspective`.
`f3-cull-blend` needs
`--scene 7e5a0030-0030-4030-8030-000000000030`.

### Desk round-trip

[`scripts/desk-verify-golden-gate.ps1`](scripts/desk-verify-golden-gate.ps1)
is the local version: a deliberate break, then a bless, then a pass.
It needs a display and a real GPU. Mutations sit in `try/finally` and
are restored with `git checkout`. It is safe to interrupt.

### Witness harness

The witness cases launch the real runtime, editor, and server and
assert reports and, where tagged, golden images. Runtime and editor
cases carry `[witness]`. Many editor cases also carry `[gpu]`. Server
cases carry `[server]`. H1 (a scripted hang) and G1 (a scripted GPU
fault) need a Windows host with `ArcaneCrashReporter` beside it. The
design note is
[docs/specs/2026-09-03-host-witness-harness-design.md](docs/specs/2026-09-03-host-witness-harness-design.md).
Later cases extend it. The test names are the list.

### Fuzzing

Five libFuzzer targets, built standalone with Clang on Linux:
`protocol`, `artifact`, `scene`, `sprite`, and `cli`. They cover the
TCP framing and JSON protocol, `.arcart` reads, scene JSON, sprite
files, and command-line parsing (including `ArcaneServer`'s).
[docs/fuzzing.md](docs/fuzzing.md) has the invariants and the corpus.

```bash
scripts/run-fuzz.sh regress all
scripts/run-fuzz.sh protocol 3600
```

`regress` replays the checked-in corpus and regressions. A CI smoke
job for that command is described in the doc and is not wired into
the workflow yet.

### CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) is the hosted
matrix: `windows-msvc`, `linux-gcc-14`, `linux-clang-19`, and
`macos-appleclang`, each for Debug, Release, and Dist. Every leg runs
the CPU suite on two seeds. Extra Debug lanes: lavapipe `[gpu]` and
the windowed smoke on `linux-gcc-14`, and MoltenVK `[gpu]` on
`macos-appleclang`. There is no ASan or TSan leg.

The Linux legs run inside the repo's CI image (`ci/linux/Dockerfile`). To run
the same build and tests on the desk before pushing: `scripts\ci\local.ps1`
(see [docs/ci.md](docs/ci.md#local-runs)).

The [Jenkinsfile](Jenkinsfile) defines the Windows real-GPU gate:
all three configurations, the unfiltered suite (including `[gpu]`),
`arccook --check`, the golden gate (Release then Debug), the self-test
on `main` and `milestone/*`, the sample built through the SDK, and
`ArcaneRuntime --frames 180`. Its "Linux lane" step only prints
toolchain versions when a Linux agent is online. It does not build.
Retiring Jenkins in favor of one GitHub Actions setup is still
planned. Standing the Jenkins job up once is [ci/README.md](ci/README.md).

### For agents

Agents use the same command lines as everyone else: `arcbuild`,
`arccook`, host flags (`--frames`, `--settle`, `--compare`,
`--probe`), `golden-gate.ps1`, and the JSON reports.

Checked-in skills under [`.claude/skills/`](.claude/skills):

| Skill | Use it for |
|---|---|
| `arcane-build` | Generate and build order, the single game-module slot, `arcbuild`. |
| `arcane-tests` | Running `ArcaneTests`, seeds, which lane ran what. |
| `arcane-verify` | The golden gate, bless rules, copying a bless back to source. |
| `arcane-graph` | What connects to a symbol, via the knowledge graph. |

`graphify-out/` is gitignored. Build it with `/graphify .` and refresh
with `/graphify . --update`. Scope is the root `.graphifyignore`.
Neighbors of a symbol, source and docs together:

```bat
python .claude\skills\arcane-graph\neighbors.py ViewTransform
```

Tooling is CLI-first. Asking a running host a new question is the job
of the Servitor CLI, described under the roadmap. The draft is
[docs/superpowers/specs/2026-10-05-arc-cli-design.md](docs/superpowers/specs/2026-10-05-arc-cli-design.md).

Layout, build rules, and the engine-as-SDK contract are also in
[CLAUDE.md](CLAUDE.md).

## Roadmap

Engine milestones only. Dates below are the ones recorded for this
body of work: the end of January 2027, with February an acceptable
slip. The order is the current one.

### Shipped

- Editor shell: docking, inspector, outliner, asset browser, undo,
  shader and material graphs, project format, and the Hub.
- Hot reload, the ABI gate, and `arcbuild`.
- NRI, the render graph, bindless materials, the GPU scene, compute
  mesh culling, and GPU crash diagnostics on Windows.
- Astra as the ECS, with 32-bit entity ids by default.
- Manifold2D, including pull-model contact events. The parallel solver
  is off unless `physics.parallelSolver` is set.
- Texture and mesh cooking, scene load/save, headless hosts.
- The witness harness and the fourteen-lane golden gate.
- Settings and cvars, including editor preferences, project settings,
  the console, and the server admin console.
- The flat gameplay API.
- Visibility and the GPU scene (F3), and 3D editor authoring (F4):
  perspective, one view transform, a 3D gizmo.
- Hosted CI that builds and tests on Windows, Linux, and macOS.
  Interactive and real-GPU use on Linux and macOS is still unverified.
  The Windows Jenkins gate is still the real-GPU golden gate.

### In progress

**Unified world rendering (F5).** World-space 2D moves onto the mesh
path (the same cull and draw path as meshes). UI and overlays stay on
Batcher2D. The work reserves an extract boundary between game state and
the renderer, one Surface material domain, multiple views, world-space
UI as a quad, and slots for TAA jitter and per-object motion vectors,
including a per-camera option with no jitter. This is underway on a
separate branch and is not in this tree. Sprites in this tree still
draw with Batcher2D.

**Servitor CLI.** A local command-line client (`servitor`; the engine
side is the ArcaneRemote module) that attaches to a running editor,
runtime, or headless host (or launches one). The planned first
slice is query, act, screenshots, and JSON, local-only, with a session
token. The checked-in design is still marked draft. The tool is not
implemented in this tree.

### Next

1. **Finish F5**, as scoped above.
2. **64-bit large-world coordinates (D17).** Positions become 64-bit.
   Rendering is camera-relative. The world origin can rebase. This is
   end to end: physics, render, assets, editor, and (once it exists)
   replication. Alongside it, in Astra (the Astra 1.0 work): 64-bit
   entity ids (32-bit index, 32-bit version), component ids that use
   dense arrays for a hot range and a lookup past it, so the component
   count stops being a hard limit (today's default limit is 128), and
   a pass that exposes real runtime tunables. Transforms in this tree are still 32-bit floats.
3. **Box3D, the minimum.** Vendor Box3D with large-world double
   positions, rigid bodies, spatial queries, and the threading seam.
   The engine does not ship a character controller: games build their
   own movers on the spatial queries. The near-term slice is bodies,
   queries, and the seam.
4. **Replication v1.** Per-entity authority, snapshots, interpolation,
   client prediction and reconciliation, and relevance. `INetDriver`
   starts as loopback and TCP, then GameNetworkingSockets. The target
   is a high player count with no fixed player cap. Snapshot positions
   are 64-bit, so this follows D17. Design:
   [docs/specs/2026-09-17-replication-v1-design.md](docs/specs/2026-09-17-replication-v1-design.md).

### Later

These are the remaining engine rows. They are planned. They are not
scheduled ahead of the list above.

- **Retained game UI.** Layout, styling, MSDF text, localization,
  gamepad and keyboard navigation, data binding, and a UI editor.
  ImGui stays the editor and the engine overlays.
- **Audio.** A mixer and bus graph on miniaudio, spatialization,
  streaming, cues, and a small editor.
- **VFX.** GPU particles: an emitter graph, sprite, ribbon, and mesh
  renderers, depth collision, and a preview.
- **Skeletal animation.** Import, GPU skinning, clips, blend spaces, a
  state machine, root motion, and IK.
- **Deadlock-class renderer.**
  - Foundation: clustered deferred lighting, PBR metal/roughness,
    cascaded sun shadows and local-light shadows, GTAO, TAA, one
    upscaler interface with FSR 3 first, bloom, exposure, and tonemap,
    on the single Surface domain. The recorded budget is 1080p, a
    144 fps floor with 256 dynamic lights and 5k draws, and 240 fps
    as the stretch goal.
  - World-scale look: baked GI, reflections, LOD and HLOD, sky,
    atmosphere, and fog.
  - A render thread and parallel command recording.
  - The feature contract is
    [docs/research/2026-08-12-deadlock-render-target.md](docs/research/2026-08-12-deadlock-render-target.md).
- **World and streaming.** Async asset loading and level/cell streaming.
- **Terrain provider seam.** One terrain-provider interface (heightfield,
  cube-sphere quadtree, or authored) with streaming and LOD hooks,
  collision, and an atmosphere-scattering pass; 1.0 targets a
  demo-quality cube-sphere planet.
- **Gameplay frameworks (engine-level, game-agnostic).** An
  inventory/item and stat-modifier framework, a building and
  snap-socket framework with stability and damage, and seeded
  procedural-generation utilities. Game-specific rules stay in each
  game.
- **Performance gates.** Job-pool utilization made visible, an async I/O
  lane, and per-system frame budgets enforced as gates.
- **Also planned, after the items above have room:**
  a Recast/Detour navmesh, Tracy wired through the engine (it is
  vendored and not called from engine code today), packaging beyond
  CI, a persistence seam, moving entity authority between server
  processes, server-authoritative hit validation with lag
  compensation, and a first-person animation rig. Astra's longer
  hardening (entity remap, per-entity snapshot and restore, moving
  entities between registries, relationship storage, replication change
  ticks) belongs to the same
  Astra track as the 64-bit entity work.

One CI system, with the Windows GPU desk as a self-hosted runner and
Jenkins retired, is planned and not done.

## Libraries

[Astra](ThirdParty/Astra) (ECS), [Manifold2D](ThirdParty/Manifold2D)
(2D physics), and [Mosaic](ThirdParty/Mosaic) are MIT. Mosaic is the
zero-dependency foundation the other two sit on: the job-scheduler
seam (`IWorkScheduler`; Mosaic creates no threads), SIMD helpers, and
the log and assert seams. The engine injects its enkiTS pool through
that seam.

Their upstream repositories are the place to change them. This repo
vendors copies. Sync with `scripts/sync-vendor.ps1 -Library Astra`
(the same script takes `Manifold2D` and `Mosaic`).

Other vendored code keeps its own licenses. [NOTICE.md](NOTICE.md)
lists them. [ThirdParty/README.md](ThirdParty/README.md) covers the
Premake wrappers (`THIRDPARTY_STATICRUNTIME`,
`THIRDPARTY_PROJECT_LOCATION`).

## Contributing

Contributions need a Contributor License Agreement, signed once before
the first pull request is merged; see [CONTRIBUTING.md](CONTRIBUTING.md)
for how to sign. It covers later contributions. You keep the copyright. The agreement licenses
the contribution to Starworks so it can ship under the Starworks
Source License, offer commercial licenses, and relicense a version as
MIT on that version's change date.

[CONTRIBUTING.md](CONTRIBUTING.md), [CLA.md](CLA.md).

## License

Arcane is source-available under the **Starworks Source License**: the
Business Source License 1.1 with the parameters in [LICENSE](LICENSE).
[LICENSING.md](LICENSING.md) is the plain-English summary. If the two
disagree, `LICENSE` wins.

You may read, modify, and fork the source. You may make and sell games
and other interactive applications, with no royalty. You may ship the
compiled engine inside a game and run servers for that game. Plugins
and modules may be free or paid.

Distributed games carry this credit where players can find it:

> Made with the Arcane Engine. Arcane (c) Ethan Temprovich / Starworks.

Copies of the source keep `LICENSE` with them.

You may not offer Arcane, or a modified version of it, as an engine
product: an engine, editor, SDK, toolset, or runtime that other people
use to build or run their own games. A separate commercial license is
the door for that. Write to licensing@starworks.dev.

Each version converts to the MIT License four years after that version
is first publicly distributed. Revisions before commit `5c120859`
(2026-10-10) stay MIT.

Astra, Manifold2D, and Mosaic are MIT, including the copies under
`ThirdParty/`. Everything else vendored here is under the license
named in [NOTICE.md](NOTICE.md).
