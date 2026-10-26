# Contributor onboarding

Read this before your first change. It covers setup, the development
workflow every change follows (with or without Claude Code), the engineering
rules that are not yet spelled out elsewhere, the files to stay out of while
other arcs are in flight, and the starter lanes.

`CLAUDE.md` is the reference for build commands, workspace rules, tests and
Servitor. This file does not repeat it; it tells you how to work.

## 1. Setup

1. **Tools.** Visual Studio 2026 with "Desktop development with C++", Git,
   and [vcpkg](https://vcpkg.io) with `VCPKG_ROOT` set (`setx VCPKG_ROOT <path>`,
   then open a new terminal).
2. **Clone and build** (default branch is `main`):
   ```bat
   git clone https://github.com/StarworksDev/Arcane.git
   cd Arcane
   scripts\setup-vcpkg-deps.bat
   GenerateProjects.bat
   msbuild Arcane.slnx /p:Configuration=Debug /m
   ```
   Then build ReferenceProject (`CLAUDE.md` > Build) and run the suite from the
   exe directory: `Binaries\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe`.
3. **Claude Code.** Open Claude Code in the repo root and trust the folder.
   The project settings (`.claude/settings.json`) ask Claude Code to install
   the **superpowers** plugin from the official marketplace. Accept it: it
   provides the workflow skills in section 2. The repo's own skills
   (`.claude/skills/arcane-*`) load automatically.
4. **CLA.** Your first pull request asks you to sign the CLA once
   (`CONTRIBUTING.md`).

**State of `main` you may see on day one:**
- One known flaky test, `BuildDriverTest.cpp` "PROC_THREAD_ATTRIBUTE_HANDLE_LIST
  inherits only the pipe write handle" (`CHECK(sawInvalid)`), can fail on
  Windows: the child process can by chance own a handle with the same number.
  It is unrelated to your work. Re-run with a different `--rng-seed`.
- `servitor screenshot` on a headless runtime held at frame 0 shows only the
  HUD until you `step` it. A fix is in flight.

## 2. Workflow (every change)

Arcane is built spec-first. Quality and correctness come before speed: never
skip a review step or accept a known defect to keep a lane moving.

1. **Brainstorm** (`superpowers:brainstorming`). Classify the change:
   - *spike*: a feasibility question; the output is an answer, not kept code;
   - *bounded*: a small change to code that already exists; a short design
     in chat, approved before you code;
   - *architectural*: a new system or module, or a change to interfaces
     others use. This is every D-row lane.
2. **Spec** (architectural). Write
   `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md`. Cite the 1.0 D-row it
   advances (`docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md`) and
   include a **Settings** section (section 3). **The engine lead approves
   every spec before a plan is written.** Open design decisions go to the
   engine lead as explicit options, not guesses.
3. **Plan** (`superpowers:writing-plans`). Write
   `docs/superpowers/plans/YYYY-MM-DD-<topic>.md`: tasks with exact files,
   RED/GREEN test steps, and a Servitor step for every change visible in a
   running host. Have the plan reviewed (a second model or the engine lead)
   before executing it.
4. **Execute** in a worktree on its own branch
   (`superpowers:using-git-worktrees`, then
   `superpowers:subagent-driven-development` or `superpowers:executing-plans`).
   TDD for every task (`superpowers:test-driven-development`): watch each test
   fail before you make it pass. The ledger under `.superpowers/sdd/` is the
   record; record every decision you take on the plan's behalf as a
   `Ruling:` line.
5. **Verify.** The full suite from the exe directory, the golden gate when
   rendering can change (`arcane-verify` skill), and **Servitor** evidence
   for anything visible in a running host (`arcane-servitor` skill): put the
   command, exit code and artifact paths in your report.
6. **Final review and PR.** A whole-branch review, then one pull request per
   arc. In the PR description, list your rulings and the deferred minors.
   Push and merge only after the engine lead agrees.

Small fixes follow the bounded path: a short design in chat, approval, TDD,
PR.

## 3. Engineering rules (binding, not all written down elsewhere)

- **Errors are values.** Exceptions are being removed before 1.0. New code
  does not `throw`; it returns `Mosaic::Result`
  (`ThirdParty/Mosaic/include/Mosaic/Result.hpp`).
- **Events are pulled as data.** There is no callback event bus. Engine facts
  are per-step arrays or typed queues that readers pull; ECS changes are
  Added/Removed-style query filters.
- **Every tunable is a setting.** New systems expose their knobs through the
  settings/cvar system (`docs/specs/2026-09-02-cvar-system-design.md`,
  `docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md`):
  name prefix, scope (Preferences / Project), apply mode, sweep-test
  coverage. Defaults for budgets come from measurements, not guesses.
- **Long-lived references use the entity GUID** (`Arcane::Identity`), never
  an Astra handle. Live component data may hold handles, but it must declare
  them through Astra's entity visitor and serialize the GUID.
- **Astra (ECS):** its four storages (table, sparse, universal,
  relationships) stay separate. Never model relationships or hierarchy as
  ordinary components. Changes to Astra go upstream first
  (github.com/StarworksDev/Astra), then sync into `ThirdParty/Astra`.
- **Threads come from the engine `JobSystem`** (enkiTS). Do not add ad-hoc
  `std::thread`s; a library's own callback thread (an audio device) is the
  exception, and nothing on it may block or allocate.
- **Large worlds are coming (D17):** world positions move to 64-bit with
  camera-relative rendering. Do not bake float world-space assumptions into
  new systems; take positions relative to a listener, origin or tile.
- **Multi-view aware.** Do not assume one camera (or one listener) per frame;
  split views and VR (post-1.0) must stay possible.
- **Platforms:** D3D12 and Vulkan only. Windows ships first; Linux and macOS
  must keep building (CI).
- **Never force tooling.** A feature that opens or edits files offers a
  choice with sensible defaults.
- **The engine stays game-agnostic:** no game references under `Source/`.
- Workspace rules in `CLAUDE.md` > "Rules baked into the workspace" also
  apply: `/MD`, no `/fp:fast`, MKS units, UTF-8 without BOM, ASCII comments.

## 4. Coordination: files to stay out of for now

One arc is rewriting these areas: Plan C of the 3D-readiness arc
(`feat/3d-readiness-c`, GPU resources). Review fix round 1 and Plans A and B
have merged, so their files are open again. Do not edit the files below
without asking first; if your design needs a change there, raise it in the
spec. The list clears when Plan C merges to `main`. Adding a new test file
under `Tests/ArcaneTests/src/` is always fine.

- **Render resources**, in `ArcaneRender/src/Arcane/Render/`: in `Nri/`:
  `BindlessTable.*`, `NriGraphContext.*`, `NriUploadRing.*`,
  `NriTextureCache.*`, `NriMeshBufferCache.*`, `MeshResidencyBudget.hpp`,
  `RenderGraph.*`, `RenderGraphExec.cpp`, `nodes/MeshNode.*`,
  `nodes/PickOutlineNodes.cpp`; and `RenderBudgetSettings.*`. New:
  `Nri/DeviceResourceLifetime.*`, `Nri/ResidencyAccount.*`,
  `Generated/BindlessCapacity.gen.hpp`.
- **Shaders**: `data/shaders/mesh.hlsl`, `data/shaders/compile-shaders.bat`
  and `.sh`. New: `data/shaders/bindless-capacity.hlsli`.
- **Build and CI**: `scripts/ci/build.ps1`, `scripts/ci/build.sh`,
  `scripts/ci/tests/test-build-sh.sh`, `scripts/generate.bat`. New:
  `build/engine-constants.lua`, `scripts/generate.sh`,
  `scripts/generate-engine-constants.ps1` and `.sh`.

Keep pull requests small and rebase on `main` often.

## 5. Starter lanes

### D9: Audio (our own mixer on the vendored miniaudio; decided)

Done means the D9 row of the roadmap and section 9 of
`docs/superpowers/specs/2026-10-05-arcane-1.0-feature-inventory.md`: a
mixer/bus graph; 3D spatialization, attenuation and simple occlusion;
streaming; sound cues with randomization; a music system (layers,
transitions); voice-over playback; a minimal audio editor.

Start from what exists:
- `Source/Client/ArcaneAudio/` (`AudioDevice`, `AudioSettings`, `AudioTypes`,
  `Modules/ArcaneAudioModule.cpp`, `ArcaneAudio.arcmodule`): extend this module.
- `ThirdParty/miniaudio/` (vendored).
- Tests: `Tests/ArcaneTests/src/AudioDeviceTest.cpp`, `SweepAudioTest.cpp`,
  `MiniaudioImpl.cpp`.

Questions the spec must answer: the bus graph and voice limits; the sound-cue
asset type and how it cooks (`Source/Developer/ArcaneAssetPipeline`); how
gameplay triggers sounds under the pull-event rule; listeners (more than one);
deterministic tests (render the mixer offline into a buffer); the settings
(volumes, device, budgets).

### D12: AI (navmesh on vendored Recast/Detour; decided)

Done means the D12 row and section 10 of the feature inventory: Recast/Detour
navmesh generation and runtime, crowd avoidance, and behaviour/state trees
with an editor. Perception and space-game AI LOD come later.

Nothing exists yet:
- Vendor Recast/Detour under `ThirdParty/` with its own `premake5.lua` and
  `.arcmodule` (pattern: `ThirdParty/imgui/ImGui.arcmodule`), and add it to
  `NOTICE.md`.
- New engine module(s) under `Source/<Type>/<Module>/`; see any `.arcmodule`
  (for example `Source/Runtime/ArcaneSim/ArcaneSim.arcmodule`) and
  `build/modules.lua`.
- Navmesh building reads mesh data from the asset pipeline
  (`Source/Developer/ArcaneAssetPipeline/`, `Source/Runtime/ArcaneAssets/`).
  Read it; do not change the codecs (section 4).

Decisions for the engine lead before the spec is final: which target comes
first (Aphelyon's isometric overworld, the 3D space game, or both); whether
the first version is queries only, with no physics coupling (3D physics,
Box3D, lands later); and whether crowd avoidance is in the first plan.
