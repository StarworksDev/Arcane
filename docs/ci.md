# CI

Arcane's hosted CI follows the shared Starworks CI standard (Arcane, Astra,
Manifold2D): one workflow, one OS x configuration matrix on pinned images and
compilers, one build entry point and one test entry point per shell, the same
section order in this document in every repo, and every departure from the
standard listed under [Deviations](#deviations) with its reason.

The self-hosted Jenkins full gate (`Jenkinsfile`, `ci/README.md`) is separate:
it is the unfiltered, real-GPU, golden-gate run on the Windows GPU desk. This
document covers the hosted GitHub Actions workflow only.

## Overview

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on every pull
request, on pushes to `main`, and on demand (`workflow_dispatch`). Job
`build-test` is the whole matrix; extra lanes ride one leg each (see
[Extra lanes](#extra-lanes)) rather than living in a second workflow. A newer
push to the same ref cancels the older run (`concurrency: ci-<ref>`).

Every leg does the same things in the same order: fetch pinned tools and
dependencies, generate with premake5, build the engine libraries and tools,
build ReferenceProject's game module through `arcbuild` (the SDK path an
external game takes), build the hosts and ArcaneTests, run the suite with two
seeds.

## Matrix

| Leg | Runner image | Compiler | Configs |
|---|---|---|---|
| `windows-msvc` | `windows-2025` | MSVC from the newest Visual Studio on the image (`vs2026` / `Arcane.slnx`), C++23 | Debug, Release, Dist |
| `linux-gcc-14` | `ubuntu-24.04` | GCC 14 (`g++-14`), libstdc++ 14 | Debug, Release, Dist |
| `linux-clang-19` | `ubuntu-24.04` | Clang 19 (`clang++-19`) on libstdc++ 14 | Debug, Release, Dist |
| `macos-appleclang` | `macos-15` (arm64) | Apple Clang from Xcode 16.4, libc++ | Debug, Release, Dist |

## Toolchains

- **Images** are pinned by name (`windows-2025`, `ubuntu-24.04`, `macos-15`),
  never `*-latest`.
- **Windows:** `microsoft/setup-msbuild`, `GenerateProjects.bat` (vs2026),
  SDL3 through vcpkg with the repo's overlay triplet (`vcpkg-triplets/`).
- **Linux:** `g++-14` or `clang-19` from the Ubuntu 24.04 archive; the leg
  exports `CC` / `CXX` and premake gets `--cc=gcc|clang`. SDL3 (not packaged
  on 24.04) is built from a pinned release by `scripts/build-sdl3.sh`.
- **macOS:** `sudo xcode-select -s /Applications/Xcode_16.4.app` (the
  `XCODE_APP` workflow variable), `CC=clang CXX=clang++`, premake
  `--cc=clang gmake`. Builds are native arm64
  (`bin/<Config>-macosx-AARCH64-md/`; `scripts/arcane-bin-dir.sh` prints the
  path). SDL3 is built from source like Linux; MoltenVK, the Vulkan loader,
  the validation layer and DXC come from one pinned LunarG Vulkan SDK
  (`scripts/fetch-vulkan-sdk-macos.sh --system`, which installs into
  `/usr/local`, dyld's default fallback path). `VK_DRIVER_FILES` names the
  MoltenVK ICD so the leg can see no other driver.

## Build driver

premake5 5.0.0-beta8 on every OS: the vendored `ThirdParty/premake5/premake5.exe`
on Windows, `scripts/fetch-premake.sh` (version + SHA-256 pinned) on Linux and
macOS. Generation is `GenerateProjects.bat` (Windows) or
`ThirdParty/premake5/premake5 --cc=<gcc|clang> gmake` (Linux, macOS).

The POSIX build order matters: `make ArcaneCore ArcaneAssetPipeline arccook
arcbuild ArcaneClient`, then `arcbuild build --project ReferenceProject --config
<cfg> --sdk <checkout>`, then `make ArcaneServer ArcaneRuntime ArcaneEditor
ArcaneTests` (their postbuild stages ReferenceProject, `Binaries/` included,
beside each host). Windows builds the solution, then ReferenceGame through
arcbuild, then the solution again so every host restages the game module.

## Tests

```
scripts/run-tests.sh  <Debug|Release|Dist> [--gpu] [--rng-seed N]   # Linux, macOS
scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]           # Windows
```

ArcaneTests runs in random order. Each leg runs `~[gpu]` twice: seed
`SEED_A = run_number` and `SEED_B = <run_number>50021`; both appear in the
suite banner and in the JUnit file name under `test-results/`. A failure
reproduces locally with `--rng-seed N`.

Per-OS exclusions are files of `<exact test case name> | <reason>` lines that
the runner refuses to read if a line has no reason:
[`scripts/linux-test-exclusions.txt`](../scripts/linux-test-exclusions.txt) and
[`scripts/macos-test-exclusions.txt`](../scripts/macos-test-exclusions.txt).
Windows has none: GitHub sets `CI`, so CrashPathTest's reporter-spawn cases
skip themselves by design (crash-reporting spec section 6) and the
`~[gpu] hosted-ci` baselines account for those skips.

## Extra lanes

| Lane | Rides on | What it proves |
|---|---|---|
| `[gpu]` on Mesa lavapipe + pinned validation layer (`scripts/build-vvl-linux.sh`) | `linux-gcc-14` Debug | API-agnostic `[gpu]` cases on the native backend with VUIDs latched as failures; editor golden slots blessed for lavapipe |
| Windowed smoke, X11 + Wayland (`scripts/linux-windowed-smoke.sh`) | `linux-gcc-14` Debug | real windows + swapchains on both Linux display servers |
| `[gpu]` on MoltenVK (`scripts/run-tests.sh --gpu`) | `macos-appleclang` Debug | the Vulkan backend boots on MoltenVK over the runner's paravirtual Apple GPU; see [GPU lanes](#gpu-lanes) |

### GPU lanes

The macOS runner's GPU is Apple's paravirtualized Metal device ("Apple
Paravirtual device"). MoltenVK on it proves instance/device creation through
the portability path, the portability-subset feature gating (the device logs
its gaps), shader translation (SPIR-V -> MSL), the bindless material table
built update-after-set, offscreen rendering, and the runtime goldens (W1-W6
match the hardware references). It does not prove performance, and no golden
slot is blessed for it: the five editor witness goldens (E2-E6) differ there
and are excluded with that reason. The real-GPU golden gate stays on the
Jenkins desk.

Both macOS test steps run under SDL's `cocoa` driver (the runner has an Aqua
session), so windows are real NSWindows whose `CAMetalLayer` is the MoltenVK
surface; Linux runs under Xvfb with the `x11` driver.

## Supply chain

- Actions are pinned by full commit SHA with the tag in a comment
  (`actions/checkout` v4.4.0, `actions/cache` v4.3.0,
  `actions/upload-artifact` v4.6.2, `microsoft/setup-msbuild` v2.0.0).
- The workflow token is read-only: `permissions: contents: read`.
- Every download is version-pinned and SHA-256 checked by
  `scripts/sha256-check.sh`: premake, DXC (Linux), SDL3 source, the Vulkan
  validation layer (tag must resolve to `VVL_COMMIT`), and the macOS Vulkan
  SDK zip (`VK_SDK_PINNED_SHA256` in `scripts/fetch-vulkan-sdk-macos.sh`).
  Compilers come from the pinned images.

## Reproducing locally

```bash
# Ubuntu 24.04
CC=gcc-14 CXX=g++-14 ThirdParty/premake5/premake5 --cc=gcc gmake
make -j3 config=debug ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient
"$(scripts/arcane-bin-dir.sh Debug)/arcbuild/arcbuild" build --project ReferenceProject --config Debug --sdk "$PWD"
make -j3 config=debug ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
xvfb-run -a scripts/run-tests.sh Debug --rng-seed 1

# macOS (Apple silicon)
sudo xcode-select -s /Applications/Xcode_16.4.app
scripts/fetch-premake.sh && scripts/fetch-vulkan-sdk-macos.sh --system
ThirdParty/premake5/premake5 --cc=clang gmake
# ... same make / arcbuild / make sequence as Linux ...
scripts/run-tests.sh Debug --rng-seed 1
scripts/run-tests.sh --gpu Debug --rng-seed 1
```

```powershell
# Windows
scripts\setup-vcpkg-deps.bat
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
powershell -File scripts\run-tests.ps1 Debug --rng-seed 1
```

## Deviations

| Deviation | Reason |
|---|---|
| `Dist` is in the matrix (the standard is Debug + Release) | Dist is a shipped configuration here (no symbols, no `TRACY_ENABLE`) and has caught Dist-only failures (e.g. CrashPathTest's symbolization case). |
| Branch pushes other than `main` are not a trigger | Running both `push` and `pull_request` for a PR branch runs every head twice and starves the shared macOS arm64 pool; matches Astra. |
| Extra lanes ride a matrix leg instead of a separate `extra` job | The GPU lanes need the leg's own build tree (hosts, staged ReferenceProject, compiled shaders); rebuilding it in a second job would double the slowest legs. |
| No sanitizer lanes | Not yet: the engine has never been run under ASan/TSan on POSIX. Tracked as follow-up work, not a design choice. |
| SDL3 and the Vulkan validation layer are built from source on Linux/macOS | Ubuntu 24.04 does not package SDL3 and its validation layer (1.3.275) is too old; both builds are pinned and cached. |
| The Windows leg uses the vendored premake `.exe` rather than a fetched, SHA-pinned one | It is checked in (`ThirdParty/premake5/`), so the repo itself pins it. |
| The macOS [gpu] lane excludes the editor golden witnesses (E2-E6) | MoltenVK on the paravirtual device rasterises editor text differently from the Windows desk's reference; see `scripts/macos-test-exclusions.txt`. |
