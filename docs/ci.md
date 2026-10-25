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
request, on pushes to `main`, and on demand (`workflow_dispatch`). A newer
push to the same ref cancels the older run (`concurrency: ci-<ref>`). Its jobs:

| Job | What it does |
|---|---|
| `image-tag` | Runs the CI scripts' own tests, then computes the Linux image tag (`scripts/ci/image-tag.sh`) |
| `ci-image` | Reuses `ghcr.io/starworksdev/arcane-ci:<tag>` when that tag is already published. Only a non-pull-request run on `main` builds a missing tag, verifies it, and then pushes (`scripts/ci/ci-image.sh`). A pull request whose image inputs changed builds and verifies the image locally, does not push it, and outputs an empty digest. The only job with `packages: write` |
| `build-test-linux` | The two Linux legs, inside that image pinned by digest. Skipped when the digest is empty (a pull request that verified a new tag locally, or a failed `ci-image`) |
| `build-test` | The Windows and macOS legs, on the hosted runners |
| `notify` | Discord FAILED / RECOVERED; never for pull requests |

Every leg does the same things in the same order, from scripts rather than
YAML (`scripts/ci/build.sh` / `scripts/ci/build.ps1`, then the test entry
points): generate with premake5, build the engine libraries and tools, build
ReferenceProject's game module through `arcbuild` (the SDK path an external
game takes), build the hosts and ArcaneTests, run the suite with two seeds.
`scripts/ci/local.ps1` runs the same scripts on the desk ([Local
runs](#local-runs)).

## Matrix

| Leg | Runner image | Compiler | Configs |
|---|---|---|---|
| `windows-msvc` | `windows-2025` | MSVC from the newest Visual Studio on the image (`vs2026` / `Arcane.slnx`), C++23 | Debug, Release, Dist |
| `linux-gcc-14` | `ubuntu-24.04` host; the leg runs in the `arcane-ci` image ([Image](#image)) | GCC 14 (`g++-14`), libstdc++ 14 | Debug, Release, Dist |
| `linux-clang-19` | `ubuntu-24.04` host; the leg runs in the `arcane-ci` image ([Image](#image)) | Clang 19 (`clang++-19`) on libstdc++ 14 | Debug, Release, Dist |
| `macos-appleclang` | `macos-15` (arm64) | Apple Clang from Xcode 16.4, libc++ | Debug, Release, Dist |

## Toolchains

- **Images** are pinned by name (`windows-2025`, `ubuntu-24.04`, `macos-15`),
  never `*-latest`. The Linux legs' container image is pinned by digest (see [Image](#image)).
- **Windows:** `microsoft/setup-msbuild`, `GenerateProjects.bat` (vs2026),
  SDL3 through vcpkg with the repo's overlay triplet (`vcpkg-triplets/`).
- **Linux:** the legs run inside the repo's CI image ([Image](#image)):
  `g++-14` / `clang-19` from the Ubuntu 24.04 archive (one apt snapshot), the
  pinned SDL3, premake, DXC and Vulkan validation layer baked in. The leg
  exports `CC` / `CXX`; `scripts/ci/build.sh` gives premake `--cc=gcc|clang`
  from `CC`.
- **macOS:** `sudo xcode-select -s /Applications/Xcode_16.4.app` (the
  `XCODE_APP` workflow variable), `CC=clang CXX=clang++`, premake
  `--cc=clang gmake`. Builds are native arm64
  (`Binaries/<Config>-macosx-AARCH64-md/`; `scripts/arcane-bin-dir.sh` prints the
  path). SDL3 is built from source like Linux; MoltenVK, the Vulkan loader,
  the validation layer and DXC come from one pinned LunarG Vulkan SDK
  (`scripts/fetch-vulkan-sdk-macos.sh --system`, which installs into
  `/usr/local`, dyld's default fallback path). `VK_DRIVER_FILES` names the
  MoltenVK ICD so the leg can see no other driver.

## Build driver

premake5 5.0.0-beta8 on every OS: the vendored `ThirdParty/premake5/premake5.exe`
on Windows; on Linux the CI image's copy (`/opt/arcane-tools`, linked into
`ThirdParty/premake5/premake5` by the build script); on macOS
`scripts/fetch-premake.sh` (version + SHA-256 pinned).

The build sequence is two scripts with the same four steps and log names
(`logs/build-{generate,engine,referencegame,tests}.log`); each stops at the
first failing step and exits with its code:

| Step | `scripts/ci/build.sh <cfg> [-j N]` (Linux, macOS) | `scripts/ci/build.ps1 <cfg> [-Jobs N]` (Windows) |
|---|---|---|
| generate | `premake5 --cc=<gcc\|clang> gmake` | `GenerateProjects.bat` (vs2026) |
| engine | `make -k ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient ArcanePhysics2D arcane-abi-digest` (then `arcane-abi-digest --check`) | `msbuild Arcane.slnx` |
| game | `arcbuild build --project ReferenceProject --config <cfg> --sdk <checkout>` | the same |
| hosts | `make -k ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests` (their postbuild stages ReferenceProject, `Binaries/` included) | `msbuild Arcane.slnx` again, so every host restages the game module |

`build.sh` is bash 3.2-safe (macOS `/bin/bash`) and calls no git (desk
worktrees' `.git` file points at a Windows path a container cannot see).
`scripts/ci/tests/test-build-sh.sh` checks it against stub tools.

## Tests

```
scripts/run-tests.sh  <Debug|Release|Dist> [--gpu] [--rng-seed N]   # Linux, macOS
scripts/run-tests.ps1 <Debug|Release|Dist> [--rng-seed N]           # Windows
```

ArcaneTests runs in random order. Each leg runs `~[gpu]` twice: seed
`SEED_A = run_number` and `SEED_B = (run_number + 50021) mod 4294967296`; both
appear in the suite banner and in the JUnit file name under `test-results/`.
A failure reproduces locally with `--rng-seed N`.

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
| `arccook --check` + ReferenceProject standalone build (`scripts/ci/reference-checks.ps1`) | `windows-msvc` Debug (both), Release (cook check) | the cooked sample content is current; a game workspace builds without arcbuild (moved from the Jenkins gate) |

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

## Image

The Linux legs run in `ghcr.io/starworksdev/arcane-ci`, built from
[`ci/linux/Dockerfile`](../ci/linux/Dockerfile) (target `build`): Ubuntu
24.04 pinned by digest, apt resolved from one Ubuntu archive snapshot
(`APT_SNAPSHOT`), the compilers and libraries the legs need, and SDL3, the
Vulkan validation layer, premake and DXC from the repo's pinned scripts. It
holds tools only, never the Arcane source. The default user is root (GitHub's
container jobs expect it); user `arcane` (uid 1000) is what local runs use.

**Tag:** `build-<16 hex>` = SHA-256 over the Dockerfile and every repo file it
copies, listed in `ci/linux/image-inputs.txt`, CR-insensitive
(`scripts/ci/image-tag.sh`; the PowerShell twin `image-tag.ps1` prints the
same tag). The scripts refuse a Dockerfile that copies a file outside the list
or a list entry the Dockerfile does not copy; repo files enter only through
one-line `COPY --from=scripts <file>... <dest>` (the build context `scripts`
is the repo's `scripts/`). Building by hand:

```
docker build --target build --build-context scripts=scripts -f ci/linux/Dockerfile -t arcane-ci:dev ci/linux
docker run --rm -v "$PWD/ci/linux:/v:ro" arcane-ci:dev bash /v/verify-image.sh
```

**Publishing:** `ci-image` reuses a published tag. Only a non-pull-request run
on `main` (a push to `main`, or `workflow_dispatch` on `refs/heads/main`)
builds a missing tag, verifies it (`verify-image.sh`) and only then pushes it
(`scripts/ci/ci-image.sh`), so a failed verify publishes nothing. `PUBLISH` is
that condition. The legs that run use `image@sha256:<digest>`, never the tag:
the tag names the inputs, the digest names the image. The package is **public**
(Arcane is public, the image holds no secrets), so pulls need no credentials;
the legs still pass the workflow token so a not-yet-public package also works.
A published tag is reused, so a later edit of `scripts/ci/ci-image.sh` (not a
tag input) does not rebuild or replace it.

**Pull requests do not publish.** A pull request whose image inputs changed
builds and verifies the image locally, does not push it, and writes an empty
digest. The Linux legs that need the registry image are skipped until a
non-pull-request run on `main` publishes the tag. A pull request whose tag is
already published reuses that digest and runs the Linux legs.

**Fork pull requests** cannot publish, and a missing tag is not built:
`ci-image` fails naming the tag. A Dependabot pull request is treated the same
way (`SAME_REPO` stays false). The image is published by the non-pull-request
`main` run after the change is reviewed and merged, so the `ci/linux/` and
`scripts/` diff is reviewed before that push.

**Retention:** an image-input change publishes a tag only from a
non-pull-request run on `main`. A pull request verifies locally and does not
publish. Nothing deletes old tags yet; the package size is watched (cleanup
deferred).

## Local runs

`scripts\ci\local.ps1` runs the CI legs on the desk before a push, from the
same scripts:

```
scripts\ci\setup-desk.ps1                     # once: .wslconfig 24 GB / 16 CPUs, Docker + MSBuild checks
scripts\ci\local.ps1                          # windows-msvc Debug + linux-gcc-14 Debug
scripts\ci\local.ps1 -Leg linux-clang-19 -Config Release
scripts\ci\local.ps1 -Full                    # windows-msvc, linux-gcc-14, linux-clang-19 x Debug/Release/Dist
         [-Seeds 1|2] [-Seed N] [-Jobs N] [-NoPull]
```

- **Windows legs** run `build.ps1` and `run-tests.ps1` natively in the checkout.
- **Linux legs** run in the CI image. The image is pulled by tag (`image-tag.ps1`);
  when it cannot be pulled, or with `-NoPull`, it is built locally and the
  summary labels it `<tag>-local (local image)` -- a pass on a local image is
  not yet parity with CI's. The checkout is mounted read-only and mirrored
  into a volume per leg (`arcane-src-<leg>`) by `scripts/ci/container-leg.sh`
  through `scripts/ci/rsync-filter.txt`: no `.git`, no build outputs, no
  Windows tools, and the leg's own outputs survive (incremental). Compiles go
  through ccache (volume `arcane-ccache`; local only).
- **Uncommitted changes are included.** Legs run one at a time (Windows first)
  and every leg runs even when one fails. Seeds: one random seed by default
  (printed); `-Seeds 2` runs CI's pair, `N` and `(N + 50021) mod 4294967296`;
  `-Seed N` fixes N (CI's N is the run number). Jobs: Linux
  `min(CPUs, memory GB / 2)` of what Docker reports, Windows `msbuild /m`;
  `-Jobs` sets both.
- **Output:** one table (leg, config, verdict, minutes, build, per-seed test
  counts, image, log directory), the first compiler/linker errors of a failed
  build, an OOM hint when the build logs show a compiler or linker killed by
  signal 9; logs under `logs\local\<leg>-<Config>\`.
  Exit 0 all passed, 1 a leg failed, 2 a preflight check failed (Docker not
  running, MSBuild missing -- each names its fix).
- **Line endings:** a Windows checkout with `core.autocrlf` holds text files as
  CRLF that CI's Linux checkout has as LF. `local.ps1` lists them with git on
  the desk and each Linux leg converts them after its mirror, so the leg builds
  and tests CI's bytes (the container itself runs no git).
- **Docker Desktop's Resource Saver** pauses the VM after a few idle minutes,
  which is shorter than a Windows leg, and some Docker Desktop versions resume
  wedged (API 500). Each Linux leg re-checks Docker first and, if it stays
  unresponsive, reports "Docker Desktop is not responding" instead of building.
  Restart Docker Desktop, and turn Resource Saver off (Settings > Resources) or
  update Docker Desktop; `setup-desk.ps1` warns while it is on.
- macOS legs run on the hosted runner only.

`scripts\ci\compare-test-counts.ps1 -Baseline <dir> -Candidate <dir>`
compares two downloaded sets of `test-results-*` artifacts (or a local
`logs\local\...\test-results`) by `<leg>-<Config>`.

## Supply chain

- Actions are pinned by full commit SHA with the tag in a comment
  (`actions/checkout` v7.0.1 with `persist-credentials: false`, `actions/cache` v6.1.0, `actions/upload-artifact` v7.0.2, `microsoft/setup-msbuild` v3, `docker/login-action` v4.6.0, `docker/setup-buildx-action` v4.4.1, and the org's `discord-ci-notify`). Cache keys are per image, never per configuration, for configuration-independent contents.
- The workflow token is read-only (`permissions: contents: read`); `ci-image` alone adds `packages: write`, and the Linux legs `packages: read`.
- Every download is version-pinned and SHA-256 checked by
  `scripts/sha256-check.sh`: premake, DXC (Linux), SDL3 source, the Vulkan
  validation layer (tag must resolve to `VVL_COMMIT`), and the macOS Vulkan
  SDK zip (`VK_SDK_PINNED_SHA256` in `scripts/fetch-vulkan-sdk-macos.sh`).
  Compilers come from the pinned images.
- The Linux image's base is pinned by digest and its apt packages by one archive snapshot (`APT_SNAPSHOT` in `ci/linux/Dockerfile`).

## Reproducing locally

On the desk, `scripts\ci\local.ps1` is the one command (see [Local runs](#local-runs)).
By hand, the sequences are the build scripts:

```bash
# Ubuntu 24.04
CC=gcc-14 CXX=g++-14 scripts/ci/build.sh Debug -j3
xvfb-run -a scripts/run-tests.sh Debug --rng-seed 1

# macOS (Apple silicon)
sudo xcode-select -s /Applications/Xcode_16.4.app
scripts/fetch-premake.sh && scripts/fetch-vulkan-sdk-macos.sh --system
scripts/ci/build.sh Debug -j2
scripts/run-tests.sh Debug --rng-seed 1
scripts/run-tests.sh --gpu Debug --rng-seed 1
```

```powershell
# Windows
scripts\setup-vcpkg-deps.bat
scripts\ci\build.ps1 Debug
powershell -File scripts\run-tests.ps1 Debug --rng-seed 1
```

## Deviations

| Deviation | Reason |
|---|---|
| `Dist` is in the matrix (the standard is Debug + Release) | Dist is a shipped configuration here (no symbols, no `TRACY_ENABLE`) and has caught Dist-only failures (e.g. CrashPathTest's symbolization case). |
| Branch pushes other than `main` are not a trigger | Running both `push` and `pull_request` for a PR branch runs every head twice and starves the shared macOS arm64 pool; matches Astra. |
| Extra lanes ride a matrix leg instead of a separate `extra` job | The GPU lanes need the leg's own build tree (hosts, staged ReferenceProject, compiled shaders); rebuilding it in a second job would double the slowest legs. |
| No sanitizer lanes | Not yet: the engine has never been run under ASan/TSan on POSIX. Tracked as follow-up work, not a design choice. |
| The Linux legs run in a repo-defined container image (`ci/linux/Dockerfile`) instead of on the bare runner image | One toolchain for hosted CI and the desk (`scripts/ci/local.ps1`); SDL3 and the validation layer (not packaged / too old on 24.04) are built once per image instead of per leg. Astra and Manifold2D may adopt the pattern. |
| macOS still builds SDL3 from source per runner (cached) | No macOS containers; the hosted macOS image is the environment. |
| The Windows leg uses the vendored premake `.exe` rather than a fetched, SHA-pinned one | It is checked in (`ThirdParty/premake5/`), so the repo itself pins it. |
| The macOS [gpu] lane excludes the editor golden witnesses (E2-E6) | MoltenVK on the paravirtual device rasterises editor text differently from the Windows desk's reference; see `scripts/macos-test-exclusions.txt`. |
