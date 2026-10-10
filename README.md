# Arcane

A C++23 game engine for Windows: NRI renderer on D3D12 + Vulkan,
game-as-DLL hot reload, an ImGui editor, a data-driven runtime host, and a
project launcher.

https://starworks.dev/arcane

## Modules

| Project | What it is |
|---|---|
| `ArcaneCore` | Static lib: networking, types, logging, shared header-only utilities. Namespaced include root (`#include <Arcane/...>`), zero game references -- liftable into any project. |
| `ArcaneClient` | The engine DLL: SDL3 window/input, NRI device (D3D12 + Vulkan, 2 frames in flight), linear-HDR Canvas -> sort-keyed Batcher2D -> ACES tonemap, MSDF text, asset cache, ImGui integration, enkiTS job system, ECS runtime (Astra), exposed to game code as the `Arcane::` facade, 2D physics (Manifold2D), scene save/load, plugin host. |
| `ArcaneRuntime` | Standalone runtime host: opens an `.arcproj` and runs its game module. `--frames N` is the scripted GPU-verify; F5 = reload with state, F6 = fresh reload. |
| `ArcaneEditor` | The ImGui-on-NRI editor host (`ArcaneEditor.exe`). |
| `ArcaneHub` | Tauri-based project launcher; owns the `.arcproj` file association and recents. |
| `ArcaneServer` | Server-side engine tooling (scaffold). |
| `ArcaneTests` | Catch2 test suite (plus hot-reload fixture plugin DLLs). |
| `ReferenceProject` | The in-repo sample game, built exactly like an external project through the SDK (`build/arcane.lua`) -> `Binaries/ReferenceGame.dll`. |

## Building

Prerequisites: Visual Studio 2026 with "Desktop development with C++",
[vcpkg](https://vcpkg.io) (`VCPKG_ROOT` set). premake5 ships bundled at
`ThirdParty/premake5/`.

```bat
scripts\setup-vcpkg-deps.bat   # once: SDL3 via the bundled overlay triplet
GenerateProjects.bat            # premake5 -> Arcane.slnx
msbuild Arcane.slnx /p:Configuration=Debug /m
```

Run the tests (from the exe dir; `~[gpu]` skips GPU-touching cases on
machines without a capable GPU):

```bat
cd bin\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe ~[gpu]
```

Build and run the sample:

```bat
cd ReferenceProject
..\ThirdParty\premake5\premake5.exe vs2026
msbuild ReferenceProject.slnx /p:Configuration=Debug /m
cd ..
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject
```

### Linux (GCC 14 / Clang 19)

The hosted `.github/workflows/linux.yml` lane is the reference recipe. The
minimum toolchains are GCC 14 or Clang 19 with libstdc++ 14 (GCC 13 lacks
C++23 deducing-this; Clang 18 cannot use libstdc++'s `<expected>`). SDL3
comes from the system (`pkg-config sdl3`, or `SDL3_ROOT`), so no vcpkg is
needed. Ubuntu 24.04 does not package SDL3, so build 3.2.x from source into
`/usr/local`. The Vulkan backend is the only one; D3D12 is Windows-only.

```sh
scripts/fetch-premake-linux.sh   # once: ThirdParty/premake5/premake5 (gitignored)
scripts/fetch-dxc-linux.sh       # once: ThirdParty/tools/dxc-linux (shaders + runtime compiles)
ThirdParty/premake5/premake5 gmake          # --cc=clang for Clang
make -j3 config=debug ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient
bin/Debug-linux-x86_64-md/arcbuild/arcbuild build --project ReferenceProject --config Debug --sdk "$PWD"
make -j3 config=debug ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
xvfb-run -a env SDL_VIDEODRIVER=x11 scripts/run-linux-tests.sh Debug   # ~[gpu] minus scripts/linux-test-exclusions.txt
```

**GPU lane and windowed hosts (Mesa lavapipe).** `[gpu]` cases and real
windows run on a GPU-less box through lavapipe, under Xvfb (X11) or a headless
weston (Wayland). Build the validation layer once with
`scripts/build-vvl-linux.sh` (installs to `/usr/local`; do NOT also install
Ubuntu's `vulkan-validationlayers` -- its 1.3.275 sync validation ignores host
timeline-semaphore waits and fails every frame-slot reuse as a hazard):

```sh
xvfb-run -a -s "-screen 0 1920x1080x24" env SDL_VIDEODRIVER=x11 scripts/run-linux-tests.sh --gpu Debug
xvfb-run -a -s "-screen 0 1920x1080x24" scripts/linux-windowed-smoke.sh Debug logs/windowed x11 wayland
```

lavapipe is a software adapter, so a `--compare` resolves its own reference
set first, `ReferenceProject/Verify/References/vulkan-lavapipe/`, then the
usual `vulkan/` and shared levels; a `--bless` on lavapipe writes only that
set (`Arcane::ReferenceAdapterSet`, `ReferenceImages.hpp`).

Keep `-j` low on small machines: the Debug ArcaneTests link alone writes
about 500 MB. The game module is built before the hosts because their
postbuild stages `ReferenceProject/` (`Binaries/` included) beside them.
Off-Windows, a module is `Name.so` (an authored `Name.dll` resolves to it),
and engine libraries are `libArcaneCore.so`/`libArcaneClient.so`.

Crash and hang reports work on Linux too (`Base/Posix/`): fatal signals on a
sigaltstack hand off to the same crash thread, stacks are walked by frame
pointer (every ELF target builds with `-fno-omit-frame-pointer`), and the
`.dmp` beside each `.txt`/`.arcdiag` is a Breakpad-format minidump --
`minidump-stackwalk` (rust-minidump) reads it, and `minidump-2-core` turns it
into a core for gdb. There is no Linux `ArcaneCrashReporter` or crash monitor
yet, so the reporter hand-off reports "failed" and `launchMonitor` is ignored.

## Automation

Two layers, both owned by the engine and both in `scripts/`, plus the agent-facing layer on top of
them (see [Agent skills and the knowledge graph](#agent-skills-and-the-knowledge-graph)).

**`ArcaneTests`** is the unit/integration suite. Run it **from its own directory** -- it resolves
data relative to the working directory and runs in random order:

```bat
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```

`~[gpu]` is the standing dev-loop filter: it excludes the 25 GPU-touching cases. It does **not**
exclude `[golden]` or `[mesh]`, which carry no `[gpu]` tag, are CPU-side, and are part of the
baseline.

**`scripts/golden-gate.ps1`** is the golden-image gate, and it is what covers what the suite
cannot: that the engine still *renders* the same picture. It runs eight lanes -- four references
(`runtime-scene`, `f3-cull-blend`, `editor-ui`, `editor-ui-perspective`), each on D3D12 and Vulkan --
launching the real hosts headless against `ReferenceProject` and comparing each capture against a
blessed reference.

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
type bin\Debug-windows-x86_64-md\golden-gate-summary.json
```

It runs in CI from the `Jenkinsfile` on a GPU agent (job setup: `ci/README.md`); GitHub Actions builds and runs `ArcaneTests`
only.

`golden-gate.ps1 -SelfTest` proves the gate is capable of failing: it deliberately breaks
`ReferenceProject`'s boot scene, asserts every lane that renders it goes red, and restores in a `try/finally` that
covers the whole mutation-to-restore window, not just its tail. The Jenkins pipeline runs it on
`main`/`milestone/*` only, immediately after the ordinary "Golden gate" stage on the same agent --
the property it proves belongs to the gate itself, which changes rarely, so it does not need to run
on every branch.

**The machine-readable contract is `golden-gate-summary.json`. Assert on `gatePassed` and the
per-lane `verdict` -- never on the exit code.** `-SelfTest` writes a SEPARATE
`golden-gate-selftest-summary.json` in the same directory (also carrying `"selfTest": true`), so a
self-test run never overwrites a green build's `gatePassed: true` artifact.

The gate deletes that file at start-up and writes it again on the way out -- including on the paths
where it *refuses to run at all* (a failed `ReferenceProject` rebuild, a malformed
`automation-exclusions.json`, a dirty tree under `-SelfTest`). A refusal is `schemaVersion: 3`,
`gatePassed: false`, a non-empty `refusalReason` and no lanes, so following the rule above never
reads a previous run's green as this run's answer.

### Blessing a reference

When a rendering change is intentional, re-bless. `--bless` accepts the converged capture as the
reference `--compare` names, writing to the level it resolved from
(`Verify\References\<backend>\<name>.png` if that override exists, else the shared
`Verify\References\<name>.png`).

**`--project` resolves against the working directory**, so where a bless lands depends on where you
run it. From the repo root it writes the SOURCE `ReferenceProject\`; from the host's exe directory
(which is how the gate runs) it writes the STAGED copy under `bin\<config>\<Host>\ReferenceProject\`.
The source tree is the one that matters: the next ReferenceProject build restages the whole project
beside the hosts and overwrites a staged-only bless. So either bless from the repo root:

```bat
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject ^
  --headless --backend dx12 --frames 60 --settle 30 --report r.json --compare runtime-scene --bless
```

or, if you blessed from the exe directory, immediately copy the blessed PNG back into the source
`ReferenceProject\Verify\References\`. Either way `git status` must show the reference modified.
`scripts\desk-verify-golden-gate.ps1` handles the restaging for its own round-trip.

Also easy to get wrong:

- **`--report` (or `--screenshot`) is required.** `--settle` is refused without one, because it
  compares captured frames and otherwise has nowhere to land the result.
- **Per-reference levels.** `runtime-scene` and `f3-cull-blend` are backend-split (a Vulkan
  override; D3D12 uses the shared file and reports `PassedOnFallback`), so bless each backend.
  `editor-ui` and `editor-ui-perspective` are shared, so bless them once. `f3-cull-blend` renders a
  fixture scene (`--scene 7e5a0030-0030-4030-8030-000000000030`) and `editor-ui-perspective` adds
  `--view-mode perspective`.

The full procedure, including the mesh-thumbnail goldens and what to do when a diff will not go
away, is the `arcane-verify` agent skill below.

### `scripts/desk-verify-golden-gate.ps1`

The desk half. CI now proves the gate **can fail** too (`golden-gate.ps1 -SelfTest`, above, on
`main`/`milestone/*`) -- this script's Phase A covers the same ground locally, with eyes on it, on
ANY branch. What only this script still covers: that blessing is **cheap** (Phase B times a full
break -> fail -> bless -> pass round-trip). Needs a display and a real GPU. Every mutation is
inside `try/finally` and restored with `git checkout --`, so Ctrl-C is safe.

### Agent skills and the knowledge graph

AI agents (Claude Code) drive the same CLIs as everyone else: `arcbuild`, `arccook`, the hosts'
`--frames`/`--settle`/`--compare`/`--probe` flags, `golden-gate.ps1` and the JSON reports. Two
additions make that reliable:

- **Project skills in `.claude/skills/`**, checked in and loaded on demand: `arcane-build`,
  `arcane-tests`, `arcane-verify` and `arcane-graph`. Each carries the repo-specific traps for its
  area (the single-slot game DLL, source-vs-staged `--project` resolution, seeds and CI lanes, where
  a bless writes). They are plain Markdown and double as human runbooks.
- **A knowledge graph of the engine** in `graphify-out/` (gitignored), built with
  [graphify](https://github.com/Graphify-Labs/graphify) over every first-party module, the Starworks
  libraries under `ThirdParty/` (Astra, Manifold2D, Mosaic) and all of `docs/`. Scope is set by the
  root `.graphifyignore`. Build it with `/graphify .` in Claude Code, refresh with
  `/graphify . --update`, and list everything connected to a symbol, code and governing docs alike,
  with:

  ```bat
  python .claude\skills\arcane-graph\neighbors.py ViewTransform
  ```

Agent tooling is deliberately CLI-first rather than MCP. Asking a *running* host a new question is
the remaining gap; it is planned as a CLI client to an in-host listener, alongside the cvar/console
work.

## Using the engine as an SDK

External game projects consume the engine in place through the `ARCANE_SDK`
environment variable (pointing at this repo root) and the premake module at
`build/arcane.lua`:

```lua
workspace "MyGame"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }
include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
arcane_game_module("MyGame")   -- SharedLib game module -> Binaries/MyGame.dll
```

The host hot-reloads the module on rebuild (debounced mtime watcher, state
preserved), with an ABI gate refusing cross-build mismatches.

### Writing gameplay code

Game code spells only `Arcane::` names. The ECS (Astra), the 2D physics
(Manifold2D) and the core library (Mosaic) are standalone libraries with
their own namespaces underneath; `<Arcane/Ecs.hpp>` and
`<Arcane/Reflection.hpp>` re-export everything a game module needs.

A **component** is reflected plain data:

```cpp
#include <Arcane/Reflection.hpp>

struct Health { float current = 100.0f; };
ARC_REFLECT_TYPE(Health)
    ARC_REFLECT_FIELD(Health, current)
        ARC_REFLECT_ATTR(Range, 0.0f, 100.0f)
ARC_END_REFLECT_TYPE()
```

A **system** declares what it touches as parameters -- component views and
engine resources -- and the scheduler orders and parallelises it from that:

```cpp
#include <Arcane/Ecs.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Physics2D.hpp>

struct Jumper : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem2D>>
{
    Arcane::ActionRef jump{"Player", "Jump"};

    void operator()(Arcane::View<Arcane::RigidBody2D>& view,
                    Arcane::Res<Arcane::Time> time,          // fixedDt, fixedStep, elapsed, ...
                    Arcane::Res<Arcane::GameInput> input,    // actions from the project's input asset
                    Arcane::ResMut<Arcane::PhysicsWorld2D> physics)
    {
        if (!input->PressedThisFixedStep(jump)) return;
        view.ForEach([&](Arcane::Entity e, Arcane::RigidBody2D& body)
        {
            const Arcane::BodyMotion2D m = physics->Motion(e, body);
            if (m.supported) physics->SetVelocity(e, body, m.velocityX, 6.0f);
        });
    }
};
```

and is registered with one line in its `.cpp`:
`ARC_SYSTEM(MyGame::Jumper, Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate)`.

A system reads 2D physics events from that same `Arcane::PhysicsWorld2D` resource.
`StepEvents()` is the most recent physics step, `FrameEvents()` gathers
every step since this frame began, and `ContactsOf(entity, out)` lists
what that entity is touching right now, sleepers included. Each side
carries the entity, its Identity GUID and the `Arcane::Collider2D` fixture
index. Keep the GUID when the identity has to outlive the frame; the
entity handle is valid only in the frame it is read (a destroyed
entity's event still carries the GUID). Spec:
`docs/specs/2026-10-08-physics-2d-events-design.md`.

- **Resources:** `Time` is present in every world. `GameInput` is present in every client world, and a server world has none. A system whose resource is missing is skipped with one log line.
- **Code outside a system** (a module's `OnUpdate`/`OnDrawUI`) reads the same data with `Registry().GetResource<Arcane::Time>()`. There are no global accessors: one process can hold several worlds (edit, Play, an embedded server, tests).
- **Templates:** the editor's *Create -> C++ Class* templates emit this shape.

### Driving it with `arcbuild`

Generating and building by hand (`premake5 <action>` then the matching build
tool) is what `arcbuild.exe` automates -- it is the one entry point the
editor's Build -> Rebuild Game Module, CI, and scripts all call instead of
re-implementing that composition. Specs:
`docs/specs/2026-09-13-arcbuild-driver-design.md` +
`docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md`.

```bat
bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe generate --project MyGame
bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build    --project MyGame --config Debug --sdk %ARCANE_SDK%
bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe probe    --project MyGame
```

`--action` picks the Premake generator/backend pair, defaulting per host
(`vs2026` on Windows, `gmake` on Linux, `xcode4` on macOS). MSBuild is the
only backend guaranteed to "just work" from a plain `Visual Studio + vcpkg`
setup. Ninja needs a Visual Studio developer environment on Windows (beta8's
`ninja` action defaults to the MSVC toolset there) and then builds the module
completely -- arcbuild stages the linked DLL into `Binaries/` itself, since
beta8's ninja action cannot run a post-build step on Windows. Make needs a
GCC/G++ toolchain (Premake beta8's `gmake` action defaults to GCC everywhere,
including Windows -- never `cl.exe`); on Windows that compiles the module but
cannot link it against the MSVC-built engine DLLs. On Linux, Make is the
default backend and builds a module against the GCC/Clang-built engine
(ReferenceProject is built this way in the Linux CI lane). Xcode resolves and composes on
every platform but only **executes** on macOS. `probe` alone needs no SDK.

## License

Arcane is source-available under the **Starworks Source License** (the
Business Source License 1.1 with Starworks parameters; each version converts to
MIT four years after release). Make and sell games with it freely; games must
credit Arcane, and Arcane may not be offered as a competing engine product.
Read [LICENSING.md](LICENSING.md) for the plain-English terms and
[LICENSE](LICENSE) for the license itself. Revisions published before the
commit that added this license remain under the MIT License.

Astra, Manifold2D and Mosaic are MIT. Vendored third-party dependencies retain
their upstream licenses; see [NOTICE.md](NOTICE.md). Contributing:
[CONTRIBUTING.md](CONTRIBUTING.md).
