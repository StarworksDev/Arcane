---
name: arcane-tests
description: Use when running ArcaneTests, reproducing an intermittent or order-dependent test failure, investigating a test crash or hang, a [gpu] test failing on validation messages, or CI reporting a coverage or baseline drop.
---

# Running Arcane tests

## Overview

ArcaneTests is a Catch2 v3 suite. It runs in RANDOM order by default and resolves data paths relative to the exe.

A green suite covers code paths, not the real hosts:
- for rendered output, use the `arcane-verify` skill;
- for live behavior in a running host, use `arcane-servitor`.

## Quick reference

The one entry point, the same one CI runs: `"~[gpu]"`, from the exe dir, with JUnit + JSON reports, then the baseline check.

```bat
powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]
```

By hand, always from the exe directory and in the foreground:

```bat
cd Binaries\Debug-windows-x86_64-md\ArcaneTests
ArcaneTests.exe                      :: full suite, incl. [gpu] (needs a capable GPU)
ArcaneTests.exe "~[gpu]"             :: no GPU on this machine
ArcaneTests.exe "<test name>" --rng-seed 1234
ArcaneTests.exe "[remote],[servitor]" :: Servitor client + ArcaneRemote host side
```

| Need | Where |
|---|---|
| The seed of a failing run | console banner `Randomness seeded to: N`; JSON reporter metadata `rng-seed`; JUnit property `rng-seed` |
| Crash or hang evidence | `<exe dir>\diagnostics\`: read the `.txt` (symbolized all-thread stacks) BEFORE theorizing; `.dmp` for WinDbg |
| Coverage guard | `scripts\check-baselines.ps1 -ReportPath <catch json> -Configuration <Cfg> -Invocation "<how it was run>"` vs `scripts\automation-baselines.json` (`run-tests.ps1` does this for you) |
| A Linux leg before pushing | `scripts\ci\local.ps1 -Leg linux-gcc-14 -Config Debug` (Docker; Ubuntu CI image, never another distro) |

## Which CI lane ran what

| Lane | Suite | Coverage check | Artifacts |
|---|---|---|---|
| GitHub Actions `ci.yml` | `{windows-msvc, linux-gcc-14, linux-clang-19, macos-appleclang}` x `{Debug, Release, Dist}`: `"~[gpu]"` minus the Linux/macOS exclusion lists (`scripts\linux-test-exclusions.txt`, `scripts\macos-test-exclusions.txt`), two seeds per leg | baseline key `"~[gpu] hosted-ci"` (hosted CI skips CrashPathTest's reporter-spawn cases by design) | `test-results\<leg>-<Cfg>-seed<N>.{junit.xml,json}` |
| GitHub Actions extra lanes | `[gpu]` on Mesa lavapipe (linux-gcc-14 Debug) and MoltenVK (macos Debug); windowed smoke (Linux X11 + Wayland) | | the job logs |
| Jenkins full gate (GPU desk, `Jenkinsfile`) | unfiltered, incl. `[gpu]` | `-Invocation "unfiltered"`: no committed baseline, reports "NO BASELINE -- not checked" | the agent workspace; archives only verify PNGs, NOT `diagnostics\` |

So:
- a Windows `[gpu]` crash came from Jenkins, and its crash files exist only on that agent until the next build cleans it;
- a coverage drop came from GitHub Actions.

Dist runs hundreds of cases fewer than Debug and Release. Every `#if !defined(ARC_BUILD_DIST)` test is compiled out, including all of ArcaneRemote/Servitor. That is not a drop: baselines are keyed per config.

## Host-spawning cases

- **What they are.** `[remote][gpu]` (`P0AcceptanceTest.cpp`, Windows only) and the `[witness]` cases launch the REAL editor, runtime and server.
- **What they need.** Built hosts AND a game module of the SAME config in `ReferenceProject\Binaries\` (see `arcane-build`'s single-slot trap). A Dist game DLL left there by `scripts\ci\build.ps1 Dist` fails the Debug server witness cases.
- **Cleanup.** They own their children through RAII guards. If you abort a run, check for leftover `ArcaneEditor`/`ArcaneRuntime`/`ArcaneServer` processes you started.
- **Desk-only lanes.** Never set `ARCANE_DIAG_DESK`: it enables lanes that deliberately crash the GPU driver (machine-wide TDR).

## What counts as a failure

- Assertions, crashes, hangs (the watchdog captures them).
- **Validation noise.** `[gpu]` tests assert `Arcane::RenderErrorCount() == 0`, which latches NRI diagnostics AND raw D3D12/Vulkan validation VUIDs. A clean assertion list with a validation message is still a failure.
- **A coverage drop.**
  - `check-baselines.ps1` fails when assertion counts go DOWN.
  - `-Invocation` is part of the key: a baseline measured under `"~[gpu]"` is not comparable with an unfiltered run.
  - A mismatch reports "NO BASELINE -- not checked" rather than passing.

## Reproducing an intermittent failure

1. Get the seed from the failing run (table above) and the configuration that failed.
2. Rerun the full suite with `--rng-seed N`.
   - Reproduces -> order-dependent. Run the failing test alone.
   - If it passes alone, list the run order with `--list-tests --order rand --rng-seed N`. Rerun the failing test together with halves of the tests before it (comma-separated names) until the trigger is isolated.
3. Does not reproduce with the same seed -> timing (jobs, GPU), not order. Loop the single test.
4. Known order hazards: a bare `Arcane::Runtime rt;` in a test (never do this), and Astra TypeContext theft between tests.

## Common mistakes

- Running from the repo root: data paths break and failures look like missing assets. Use `run-tests.ps1` or `cd` to the exe dir.
- Starting the suite in the background from a subagent: the harness can kill background children.
- Treating a green `[gpu][golden]` Catch2 case as proof the hosts render correctly. It links neither RuntimeApp nor EditorApp.
- Running the Debug suite right after a Release/Dist CI build without restoring the Debug game module.
