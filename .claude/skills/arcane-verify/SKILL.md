---
name: arcane-verify
description: Use when a change can alter rendered output (shaders, render passes, camera, UI, scene content) and must be checked on the GPU against golden images, when re-blessing references after an intentional visual change, or when golden-gate lanes fail with a diff that will not go away.
---

# Verifying rendered output

## Overview

`scripts\golden-gate.ps1` is the gate that covers what an agent actually runs: it launches the real
ArcaneRuntime and ArcaneEditor on dx12 AND vulkan with `--compare` and grades what they report.
The in-process `[gpu][golden]` Catch2 case is not a substitute. **REQUIRED BACKGROUND:**
`arcane-build` (single-slot game DLL, `--project` resolution).

## Run the gate

The gate rebuilds ReferenceProject but NOT the engine. After a shader or engine change, build
`Arcane.slnx` for the config first (the ArcaneClient prebuild recompiles the HLSL). Then:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Debug
type bin\Debug-windows-x86_64-md\golden-gate-summary.json
```

**Judging it:** pass = `gatePassed: true` and empty `refusalReason`. Per lane, the verdict is
`exitReason` in the lane's report, never the raw exit code (codes collide). Lanes run from the host
exe directory, so their files live in the STAGED project,
`bin\<Cfg>-windows-x86_64-md\<Host>\ReferenceProject\Saved\Verify\`:

- report: `golden-gate-<Host>-<backend>-<slot>-report.json`
- diff image: `<slot>-<backend>-diff.png` -- look at it; the delta must be exactly your change.

Verdicts: Passed, PassedOnFallback, Failed, Errored, NotRun, Skipped, Indeterminate.
PassedOnFallback is normal for a dx12 lane whose slot only has a vulkan override.

## The lanes

| Slot | Host | Extra args | References |
|---|---|---|---|
| `runtime-scene` | ArcaneRuntime | none | shared file = dx12, `vulkan\` override |
| `f3-cull-blend` | ArcaneRuntime | `--scene 7e5a0030-0030-4030-8030-000000000030` | shared file = dx12, `vulkan\` override |
| `editor-ui` | ArcaneEditor | none | shared only (both backends, both configs) |
| `editor-ui-perspective` | ArcaneEditor | `--view-mode perspective` | shared only |

A bless writes to the level the reference resolved from: `Verify\References\<backend>\<slot>.png`
if that override exists, otherwise the shared `Verify\References\<slot>.png`. So a dx12 bless of
`runtime-scene` or `f3-cull-blend` rewrites the SHARED file and a vulkan bless rewrites `vulkan\`.

### Software adapters (Linux CI: Mesa lavapipe)

On a SOFTWARE adapter the hosts probe one more level FIRST:
`Verify\References\<backend>-<adapter>\<slot>.png` -- `vulkan-lavapipe`, `dx12-warp`, or
`<backend>-software` (`Arcane::ReferenceAdapterSet`, keyed off NRI's
`adapterDesc.architecture == SOFTWARE`). Hardware adapters never see it. Comparison falls through
to `vulkan\` and shared when the set has no image for a slot, but **a bless on a software adapter
writes ONLY the set's own image** -- it can never touch the hardware references. The report's
`resolvedLevel` reads `"adapter"` when the set answered.

Measured 2026-10-05 (Mesa 25 lavapipe, LLVM 20): `runtime-scene`, `f3-cull-blend` and the five
`thumbs\` goldens match the hardware references at a ZERO budget, and editor text differs by at
most 2/255 on glyph edges (under the comparator's JND). So keep the set minimal: an image goes in
`vulkan-lavapipe\` only when lavapipe genuinely needs its own. The five editor slots there now exist
because the hardware editor references predate 3c4a73f (WindowPadding 8 -> 4) and are stale; once
they are re-blessed on Windows, re-run the Linux `[gpu]` lane with the five lavapipe editor images
removed and delete them if it stays green.

Blessing on Linux (from the host exe directory, as below, with `SDL_VIDEODRIVER=offscreen`):
`./ArcaneEditor --project ReferenceProject --headless --backend vulkan --frames 60 --settle 30
--report <scratch>.json --compare <slot> --bless`, then copy
`ReferenceProject/Verify/References/vulkan-lavapipe/` back to source, exactly like step 3 below. The
validation layer must be the one `scripts/build-vvl-linux.sh` builds (see the README's Linux
section): Ubuntu's packaged layer reports false sync hazards. An intentional visual change to a
slot that has a `vulkan-lavapipe\` image needs that image re-blessed on Linux too, or the Linux
`[gpu]` lane fails.

## Re-bless after an intentional change

1. Run the gate first; read every failing lane's diff image. (Running it also rebuilds and
   restages ReferenceProject, so the staged `ReferenceGame.dll` the blesses load is current.)
   Put `--report` outputs in a scratch dir such as `%TEMP%`.
2. From each host's exe directory (so `<project>` is the staged copy), once per slot and, for the
   runtime slots, once per backend:
   `ArcaneRuntime.exe --project ReferenceProject --headless --backend <b> --frames 60 --settle 30 --report <scratch>.json --compare <slot> <extra args> --bless`
   (`ArcaneEditor.exe` for editor slots; one dx12 bless covers a shared slot.)
3. **Immediately copy each blessed PNG from the staged `Verify\References\` into the matching path
   under the source `ReferenceProject\Verify\References\`.** The next ReferenceProject build
   restages the whole project beside the hosts and overwrites a bless that exists only in the
   staged tree. `git status` must now show each reference PNG modified.
4. Run the gate in BOTH Debug and Release (build `Arcane.slnx` for Release first); expect every
   lane green with 0 diff pixels. References are keyed by backend, never by config: do not bless
   again under Release; a Release-only diff is a bug. End on the config the desk normally uses
   (the last run owns `ReferenceProject\Binaries\`).

If the vulkan lane of a SHARED slot fails after a dx12 bless, the backends genuinely disagree:
bless that lane on vulkan with a `Verify\References\vulkan\<slot>.png` in place (create the folder
and seed it with a copy first, since a bless only writes the level that resolves). Mention the
split to the user.

## Other goldens a render change can move

- **Mesh thumbnails** (any lighting or tonemap change moves them): `set ARCANE_THUMBS_BLESS=1`
  then `ArcaneTests.exe "[thumbs][golden]"` from the tests exe directory. This bless writes the
  SOURCE tree directly; unset the variable and rerun to confirm green.
- **`inprocess-lit-cube.png`** (the `[gpu][golden]` Catch2 case): it has NO bless mode and never
  writes its reference. If it fails after an intended change, stop and tell the user; do not
  invent a replacement procedure.

## When a diff will not go away

- **Staged strays.** Host post-builds copy `ReferenceProject\` additively and never mirror
  deletions. A file deleted from source can linger in the staged `Source\` or `Content\` and change
  the editor capture (asset count, badges). Diff the staged trees against source and delete
  staged-only files.
- **Wrong game DLL** after a config flip without a rebuild: see `arcane-build`.
- `--settle` is refused without `--report` or `--screenshot`; `--bless` without `--compare` is an error.
- `--headless` never reads `imgui.ini`, so a desk layout file cannot skew a verify capture.

`-SelfTest` proves the gate can fail and requires a clean `ReferenceProject\`; it writes
`golden-gate-selftest-summary.json`, never the normal summary.
