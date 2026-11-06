---
name: arcane-build
description: Use when building Arcane or ReferenceProject, after adding/removing/renaming source files or editing any premake5.lua, when switching between Debug, Release and Dist, or when a host fails with "plugin: initial load failed", an access violation at module load, or unresolved symbols after a file was added.
---

# Building Arcane

## Overview

Two workspaces, one build order:
1. The engine (`Arcane.slnx`, repo root) builds first.
2. The game module (`ReferenceProject.slnx`, built over the SDK) links the engine's import libs and builds second.

Most build pain here comes from three traps:
- an unregenerated file list;
- the single-slot game DLL;
- resolving `--project` against the wrong directory.

Outputs live under `Binaries\<Cfg>-windows-x86_64-md\<Project>\` (not `bin\`).

## Quick reference (repo root unless noted)

| Situation | Command |
|---|---|
| Everything, exactly as CI does it | `powershell -ExecutionPolicy Bypass -File scripts\ci\build.ps1 <Cfg>` (generate, engine, game module, engine again to restage; logs in `logs\build-*.log`) |
| New/removed/renamed file, or any `premake5.lua` edited | `set _APH_NOPAUSE=1` then `GenerateProjects.bat` (cmd; in PowerShell `$env:_APH_NOPAUSE=1`) |
| Build the engine | `msbuild Arcane.slnx /p:Configuration=<Cfg> /m` (or `ci\msbuild.cmd` if msbuild is not on PATH) |
| Build the game module | `Binaries\<Cfg>-windows-x86_64-md\arcbuild\arcbuild.exe build --project ReferenceProject --config <Cfg> --sdk <this checkout>` |
| Game module after a config flip | the same command with `rebuild` in place of `build` |
| GPU smoke run | `Binaries\<Cfg>-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --frames 180` |
| Inspect or drive a running host | `Binaries\<Cfg>-windows-x86_64-md\Servitor\servitor.exe` (see `arcane-servitor`) |

- `arcbuild.exe` is not on PATH.
- `arcbuild probe --project ReferenceProject` reports the slot verdict without building (exit 3 = would rebuild).

## The traps

**Unregenerated file list.**
- A new `.cpp` is invisible to MSBuild until premake regenerates the solution.
- Symptom: unresolved externals, or code that silently never runs.
- Without `_APH_NOPAUSE` the script blocks on `pause` in a non-interactive shell.

**Single-slot `ReferenceProject\Binaries\`.**
- Every configuration writes the same `ReferenceGame.dll`, and whichever config built last owns the slot.
- After Debug -> Release (or any flip), an incremental build can call itself up to date and leave the other config's DLL in place. A Debug-CRT module in a Release host crashes. After a flip, always `rebuild`, never `build`.
- `scripts\ci\build.ps1 Release|Dist` also leaves ITS game DLL in the slot. A Debug test run after it fails the server witness cases until you restore the slot with `arcbuild build --project ReferenceProject --config Debug`.
- So run the Debug suite BEFORE the Release/Dist builds, or restore the slot first. End a session with the slot back on Debug.

**`--project` resolves against the working directory.**
- A relative `--project ReferenceProject` opens the SOURCE tree when run from the repo root, and the STAGED copy (`Binaries\<Cfg>...\<Host>\ReferenceProject\`) when run from the exe directory.
- The staged copy is taken by each host's post-build during the ENGINE build, before the game module is rebuilt. In the normal order, its `Binaries\ReferenceGame.dll` is stale by construction; `scripts\ci\build.ps1`'s second engine pass restages it.
- Run smoke tests from the repo root, as CI does. The golden gate is the exception: it restages and runs from the exe dir (see `arcane-verify`).

**Always build the solution, never a bare `.vcxproj`.** Project references and the shader prebuild only run through `Arcane.slnx`.

**Stale objects after a layout change.** If a struct layout or ABI-visible header changed and behavior is inexplicable, do a full rebuild before debugging.

**`ARCANE_SDK` can be stale in the process environment.**
- arcbuild resolves the engine through it, and fails with "no SDK" when it is unset.
- Pass `--sdk <this checkout's root>` explicitly rather than trusting the shell's value. This matters in worktrees, where the shell's value usually names a different checkout.

## Order for "everything, then verify"

1. `scripts\ci\build.ps1 <Cfg>` (or, by hand: regenerate if needed, then `Arcane.slnx`, then `arcbuild rebuild`, then `Arcane.slnx` again).
2. Check it:
   - tests: `arcane-tests`;
   - golden images: `arcane-verify`;
   - live behavior in a running host: `arcane-servitor`.

Shaders need no separate step: the ArcaneClient prebuild compiles `data/shaders/*.hlsl`.
