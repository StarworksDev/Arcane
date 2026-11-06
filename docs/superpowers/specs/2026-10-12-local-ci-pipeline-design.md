# Local CI pipeline: one Linux image, shared CI scripts, one local command

**Date:** 2026-10-12 · **Base:** `main` 946092fb · **Branch:** `ci/local-pipeline`
**Status:** design approved in conversation (sections 1-3); rev 2 folds in the written-spec review (s7); awaiting approval

This is sub-project **A** of the move off Jenkins (user decision 2026-10-05:
one CI system, GitHub Actions). The decomposition, agreed 2026-10-12:

| # | Sub-project | Repo |
|---|---|---|
| **A** | **This spec.** Linux build image + shared CI scripts + one local command | Arcane |
| B | Self-hosted Windows GPU runner on the desk; Arcane's Jenkinsfile retired after side-by-side parity | Arcane |
| C | Distroless (chiseled Ubuntu 24.04) server-runtime images, ArcaneServer first | Arcane |
| D | Aphelyon Server Linux port + per-service runtime images; Aphelyon CI on Actions | Aphelyon |

Order A -> B -> C -> D. The engine modularization arc (`feat/modularization`,
P6 parked uncommitted) resumes after B and runs its wholesale test on this
pipeline.

## 0. Intent

**What the user asked for:** a formal local build/test pipeline without
Jenkins, in favor of GitHub Actions -- not ad hoc commands.

**Success:**
1. One documented command at the desk runs the same build + test sequence the
   hosted CI runs, for Windows (native) and Linux (container), and reports
   one pass/fail table.
2. The hosted Linux legs run inside the same repo-defined image the local
   command uses, so "green locally" and "green in CI" mean the same thing.
3. The CI build steps live in scripts, not in YAML: `ci.yml` and the local
   command are two callers of one sequence.

**Decisions (user, 2026-10-12):**
- Local mode: BOTH a pre-push local run (no GitHub round trip) AND, in B, the
  same scripts executed by a self-hosted runner. Scripts-first; runners are callers.
- Linux environment: a repo-defined Docker image (Ubuntu 24.04, CI's exact
  toolchain), used locally AND by the hosted Linux legs (`container:`).
- Default local run: fast (windows-msvc Debug + linux-gcc-14 Debug), with
  `-Leg` / `-Config` selection and `-Full`.
- `.wslconfig` raised to 24 GB / 16 processors (the desk has 64 GB / 20
  threads; the 8 GB / 10 cap thrashed a Debug link at -j6).
- Image delivery: content-addressed tags, built on demand by CI.
- Server runtime images: full images (C, D) -- out of this spec.

**Constraints carried in:**
- `docs/ci.md` is the Starworks CI standard shared with Astra and Manifold2D:
  one workflow, one build and one test entry point per shell, departures
  listed under Deviations.
- Roadmap D26 security rules for Actions (self-hosted specifics are B's; the
  token, pinning and fork rules apply here): least-privilege `GITHUB_TOKEN`,
  third-party actions pinned to commit SHAs, nothing secret reachable from a
  fork PR.
- Server OS decision (2026-10-10): Ubuntu 24.04 LTS, GCC 14 / Clang 19 -- the
  image is that toolchain.

## 1. Components

### 1.1 `ci/linux/Dockerfile`, target `build`

Base `ubuntu:24.04`, pinned by digest. apt is pinned to an Ubuntu archive
snapshot (`APT_SNAPSHOT=<YYYYMMDDTHHMMSSZ>` declared in the Dockerfile; every
`apt-get update` / `install` runs with `--snapshot $APT_SNAPSHOT`, supported on
24.04), so the same Dockerfile resolves the same package versions on any
machine and the snapshot is part of the tag's input. Baked in:
- CI's current apt set: `g++-14`, `clang-19`, `make`, `cmake`, `ninja-build`,
  `pkg-config`, `libvulkan-dev`, `mesa-vulkan-drivers` (lavapipe), the SDL3
  X11/Wayland build deps, `libasound2-dev`, `libpulse-dev`, `libudev-dev`,
  `libdbus-1-dev`, `xvfb`, `xauth`, plus the GPU-lane extras (`weston`,
  `xdotool`, `imagemagick`, `x11-utils`, `python3`) and `ccache`, `rsync`, `git`.
- SDL3 from its pinned release via `scripts/build-sdl3.sh`, installed to
  `/usr/local` (pkg-config `sdl3` finds it; premake's discovery is unchanged).
- premake 5.0.0-beta8 (`scripts/fetch-premake.sh`) and DXC
  (`scripts/fetch-dxc-linux.sh`), SHA-256 checked, installed under
  `/opt/arcane-tools/` and exposed through `ARCANE_TOOLS_DIR` -- `build.sh`
  links them into the checkout's `ThirdParty/` locations the build expects.
- The pinned Vulkan validation layer (`scripts/build-vvl-linux.sh`, tag +
  commit pinned) for the GPU lane.
- A user `arcane` (uid/gid 1000) exists, but the image's DEFAULT user stays
  **root**: GitHub's `container:` jobs mount the workspace (`/__w`),
  `/github/home` and `/__e` owned by the runner's uid 1001, and job containers
  are expected to run as root. `local.ps1` runs its containers with
  `--user 1000:1000`.
- `/work` and `/ccache` are created in the image, owned by 1000:1000, so a
  fresh named volume mounted there inherits writable ownership on first use.

The image holds tools only -- never the Arcane source. The Dockerfile is
checked out with LF line endings (`.gitattributes`: `Dockerfile text
eol=lf`; the `*.sh` rule already exists).

### 1.2 `scripts/ci/image-tag.sh`

Prints `build-<hash>`: the first 16 hex digits of SHA-256 over the
Dockerfile and every file it copies or runs (`scripts/build-sdl3.sh`,
`scripts/fetch-premake.sh`, `scripts/fetch-dxc-linux.sh`,
`scripts/build-vvl-linux.sh`, `scripts/sha256-check.sh`), in a fixed order,
with line endings normalized to LF. The input list is checked, not trusted:
the script fails when the Dockerfile `COPY`s or `RUN`s a repo file the list
does not contain (s3.1 exercises it). The one definition of "which image": CI
and the local command both use it. Full image name:
`ghcr.io/starworksdev/arcane-ci:<tag>`.

`scripts/ci/image-tag.ps1` is its PowerShell twin, so the desk computes the
tag without starting a container; s3.1 asserts both print the same tag.

**The tag names the inputs; the digest names the image.** CI runs its legs on
`image@sha256:<digest>` (s1.6). A locally BUILT image (the `-NoPull` path, or
a tag CI has not published yet) is tagged `<tag>-local` and the local summary
says "local image" on those legs, so a local-build pass is never mistaken for
parity with CI's image.

### 1.3 `scripts/ci/build.sh` and `scripts/ci/build.ps1`

`build.{sh,ps1} <Debug|Release|Dist> [-j N]` -- today's inline `ci.yml` build
steps, unchanged in order:
1. generate (`GenerateProjects.bat` / `premake5 --cc=<gcc|clang> gmake`, the
   compiler from `CC`);
2. engine libraries + tools;
3. ReferenceGame through `arcbuild build --project ReferenceProject`;
4. hosts + ArcaneTests (Windows: the solution again, so hosts restage the module).

Logs to `logs/build-*.log` (the names `ci.yml`'s error summary greps). The
build **stops at the first failing step** and exits with that step's code.
The test entry points stay `scripts/run-tests.sh` / `scripts/run-tests.ps1`,
unchanged.

Rules for `build.sh` (it also runs on macOS and on a desk Mac, where
`/bin/bash` is 3.2):
- bash 3.2 + POSIX tools only: no associative arrays, `${var,,}`, `mapfile`;
  the CPU count is `nproc` with `sysctl -n hw.ncpu` as the fallback.
- **No git.** The Linux build path does not call git today (premake, arcbuild
  and the build/fetch scripts were checked, 2026-10-12) and must not start:
  the desk's checkouts are git worktrees whose `.git` FILE points at a
  Windows path that does not exist in the container. Revision information,
  if a later step needs it, comes in through an environment variable.
- It links the image's `/opt/arcane-tools` premake and DXC into the
  checkout's `ThirdParty/` locations each run (s1.4 keeps rsync from
  deleting or overwriting them).

### 1.4 `scripts/ci/local.ps1`

The desk's one command.

```
scripts\ci\local.ps1                         # windows-msvc Debug + linux-gcc-14 Debug
scripts\ci\local.ps1 -Leg linux-clang-19 -Config Release
scripts\ci\local.ps1 -Full                   # windows-msvc + linux-gcc-14 + linux-clang-19, x Debug/Release/Dist
         [-Seeds 1|2] [-Jobs N] [-NoPull]
```

- **Windows legs:** `build.ps1 <cfg>`, then `run-tests.ps1 <cfg> --rng-seed <seed>`, natively.
- **Linux legs:** resolve the tag (`image-tag.sh` via Docker), `docker pull`,
  else `docker build --target build -t <image> ci/linux` (`-NoPull` forces
  the local build, tagged `<tag>-local`). Then `docker run --rm --init
  --shm-size=2g --user 1000:1000` with:
  - the checkout mounted read-only at `/src`;
  - a named volume `arcane-src-<leg>` at `/work` (per leg: gcc and clang
    outputs never mix);
  - a named volume `arcane-ccache` at `/ccache` (`CCACHE_DIR`), compiler
    launch through ccache;
  - `CC`/`CXX` for the leg.
  Inside: `rsync -a --delete` `/src` -> `/work` excluding `.git` (s1.3),
  `bin/`, `bin-int/`, `.vs/`, generated project/make files, `Packages/`,
  `Intermediate/`, `logs/`, and the tool locations `build.sh` manages
  (`ThirdParty/premake5/premake5*`, `ThirdParty/tools/dxc*/` -- otherwise
  the Windows `premake5.exe` / DXC are copied in and `--delete` removes the
  links each run); then `build.sh <cfg>` and `run-tests.sh <cfg> --rng-seed
  <seed>` under `xvfb-run` (as CI does); then copy `logs/` and
  `test-results/` out to the host's `logs/local/<leg>-<cfg>/`.
  The rsync crosses Docker Desktop's Windows<->WSL file sharing and stats
  every file each run; the plan measures a no-change sync on the desk and, if
  it is slow, adds the large trees the Linux build never reads to the excludes.
- **Seeds:** one by default (printed); `-Seeds 2` runs CI's pair, `N` and
  `(N + 50021) mod 4294967296`.
- **Legs run serially** (one at a time; Windows then Linux), so a leg owns the
  machine and the job count below is meaningful. A parallel mode is not in
  this spec.
- **Jobs:** default for a Linux leg = `min(cpus, floor(memGB / 2))` of what
  Docker reports (24 GB / 16 CPUs -> 12), MSBuild `/m` for Windows; `-Jobs`
  overrides both.
- **Uncommitted changes are included** (it is a pre-push check).
- **macOS:** not runnable locally; the hosted leg only.

### 1.5 `scripts/ci/setup-desk.ps1`

One-time desk setup, idempotent:
- `.wslconfig` -> `memory=24GB`, `processors=16` (keeping other keys); warns
  that `wsl --shutdown` applies it (and stops running distros/Docker).
- checks Docker Desktop is installed and running (WSL2 backend);
- checks Visual Studio + MSBuild via vswhere.
- No GHCR login is needed to pull (the image is public, s2.4).

### 1.6 `.github/workflows/ci.yml`

- New first job **`ci-image`** (`ubuntu-24.04`): runs `image-tag.sh`;
  `docker buildx imagetools inspect` the tag. A published tag is reused.
  Only a non-pull-request run on `main` builds a missing tag, verifies it,
  and then pushes. A pull request whose image inputs changed builds and
  verifies the image locally, does not push it, and writes an empty digest;
  the Linux legs that need the registry image skip. A fork pull request
  with a missing tag fails, naming the tag. Outputs: `tag` and `digest`
  (empty on a local verify; otherwise the registry manifest's digest from
  `docker buildx imagetools inspect`). Permissions: `contents: read`,
  `packages: write` -- on this job only. Its `docker/setup-buildx-action` and
  `docker/login-action` (login with `GITHUB_TOKEN`) are pinned to commit SHAs,
  like every `uses:` (D26). A `concurrency` group keyed on the tag makes two
  runs that miss the same tag build it once (the second waits, then finds it).
- **Linux legs:** `needs: ci-image`, and only when the digest is non-empty.
  `container: { image:
  ghcr.io/starworksdev/arcane-ci@${{ needs.ci-image.outputs.digest }},
  options: --init --shm-size=2g }` -- the immutable digest, never the tag; the
  container runs as root (the image default, s1.1). The apt / SDL3 / premake / DXC / validation-layer
  steps and their caches are removed; the steps become `scripts/ci/build.sh`,
  the two-seed `run-tests.sh`, the GPU lane and the windowed smoke (still on
  the gcc Debug leg).
- **Windows / macOS legs:** unchanged runners and tool fetches; the inline
  build step becomes `scripts/ci/build.ps1` / `scripts/ci/build.sh`.
- Workflow-level `permissions: contents: read` stays the default.

## 2. Flows and failure handling

**CI run.** `ci-image` (seconds when the tag exists; one ~20 min build when
it does not) -> Linux legs in the container when the digest is non-empty;
Windows/macOS legs in parallel as today. A pull request that introduces a
new tag verifies the image in `ci-image` and skips the Linux legs until a
non-pull-request run on `main` publishes. Each Linux leg that runs still
pulls the full image (a few GB) at job start;
that pull replaces today's apt + SDL3 + tool steps and is part of the leg's
time, not of `ci-image`'s. The image is **public** in GHCR (Arcane is public and the image
holds no secrets), so hosted and local pulls need no credentials. (Aphelyon's
images in D are private.)

**Local run.** Preflight (Docker up; `.wslconfig` limits present or a
slowness warning; VS found) -- a failed check names the fix and stops before
building. Every requested leg runs even if one fails (`fail-fast: false`
semantics). The summary table: leg, config, build, tests passed/failed, seed,
duration, log path; exit code non-zero if any leg failed.

**Failures.**
- Build failure: the summary shows the first compiler/linker errors (the grep
  `ci.yml` uses).
- Container OOM: exit 137 -> the summary names it and suggests `-Jobs`.
- Ctrl-C: `--init` + `--rm` stop the container cleanly; the volumes persist
  for the next incremental run.
- Fork PR + new image tag: `ci-image` fails with the reason (s1.6). A
  same-repo pull request builds and verifies that tag locally and does not
  push. The image is published by a non-pull-request run on `main` after
  the change is reviewed and merged. The maintainer **reviews the Dockerfile
  and `scripts/` diff first**: that `main` run is the push.
- Bots: the repo has no Dependabot config today. If one is added, its PRs get
  a read-only token and fail `ci-image` on a new tag exactly like a fork PR.

## 3. Testing / acceptance

1. **Image reproducibility:** `image-tag.sh` twice on the same inputs gives
   the same tag; a content edit to any input file changes it; a CRLF-only
   change does not; a Dockerfile referencing a file missing from the input
   list fails the script; `image-tag.ps1` prints the same tag as
   `image-tag.sh`. `.gitattributes` keeps `*.sh` and `Dockerfile` LF on a
   Windows checkout (`git ls-files --eol`). A small shell test, run in
   `ci-image`, checks the script cases.
2. **Parity** -- the GPU lane and the windowed smoke FIRST (weston, Xvfb and
   lavapipe under a different user in a container are the likeliest casualty;
   the smoke script sets its own `XDG_RUNTIME_DIR`, and the image provides a
   writable runtime dir for the root user): the containerized Linux legs report the same ArcaneTests counts
   (cases / assertions) as the last green pre-container run of the same base
   commit, for gcc and clang, all three configs; the GPU lane and the windowed
   smoke pass in the container. Windows/macOS counts unchanged.
3. **Local = CI:** `local.ps1 -Leg linux-gcc-14 -Config Debug -Seeds 2` on the
   branch tip reports the same pass/fail counts as that CI leg; `local.ps1
   -Leg windows-msvc` passes `~[gpu]`.
4. **Red is visible:** a deliberately broken commit on a throwaway branch
   (never merged) turns the local summary and the CI run red with the error
   shown.
5. **Security review:** `packages: write` appears only on `ci-image`; every
   `uses:` is pinned to a commit SHA; no secret is referenced by any job a fork
   PR can trigger.

## 4. Docs

- `docs/ci.md` (shared standard): new **Image** (Dockerfile, tag scheme, GHCR,
  public visibility) and **Local runs** (`local.ps1`, `setup-desk.ps1`)
  sections; Toolchains/Build driver point at `scripts/ci/`; **Deviations**
  records that Arcane's Linux legs run in a container (a pattern Astra and
  Manifold2D may adopt later).
- `README.md` / `CLAUDE.md`: one line each -- run CI locally with
  `scripts\ci\local.ps1`.

## 5. Rollout

Branch `ci/local-pipeline` (worktree `D:/dev/starworks/Arcane-ci`) off
`main`. It reaches `main` through a PR so the hosted legs prove it, including
the first image build. Pushing and opening the PR need the user's go-ahead.
GHCR creates a package PRIVATE on its first push: right after the first
`ci-image` push, the package `arcane-ci` is switched to **public** once in
its package settings (by the user or an org admin), and linked to the Arcane
repo. Until then local pulls (and any fork-PR leg) fail; the plan has this as
an explicit step.
Then B gets its own brainstorm; then the modularization arc resumes (its CI
edits rebase onto `scripts/ci/`).

## 6. Out of scope

- ccache in hosted CI (local only for now).
- Windows or macOS containers.
- A self-hosted Linux runner (B decides whether one is wanted).
- Server-runtime / service images (C, D).
- Retiring Jenkins (B).
- GHCR retention: an image-input change publishes a tag only from a
  non-pull-request run on `main`. A pull request verifies locally and does
  not publish. Nothing deletes old tags yet. A cleanup policy (e.g. keep
  tags referenced by `main`'s last N runs) is deferred; the package's size
  is watched.

## 7. Written-spec review (2026-10-12) -- dispositions

| # | Finding | Disposition |
|---|---|---|
| 1 | non-root image user vs `container:` workspace ownership | Accepted: default user root; `local.ps1 --user 1000:1000`; `/work`, `/ccache` owned by 1000 (s1.1, s1.4) |
| 2 | worktree `.git` file points at a Windows path | Accepted as a rule: the Linux build path uses no git (verified); rsync excludes `.git` (s1.3, s1.4). `safe.directory` not needed while no git runs |
| 3 | CRLF in scripts | `*.sh text eol=lf` already in `.gitattributes` (verified `w/lf`); the extension-less Dockerfile was the real gap -> `Dockerfile text eol=lf`; checked in s3.1 |
| 4 | macOS bash 3.2 | Accepted: `build.sh` is bash-3.2/POSIX (s1.3) |
| 5 | tag addresses inputs, not the built image | Accepted: apt snapshot pinning; legs on the digest; `<tag>-local` labelling (s1.1, s1.2, s1.6) |
| 6 | hand-maintained input list drifts | Accepted: the script refuses a Dockerfile reference outside its list (s1.2, s3.1) |
| 7 | default job count OOMs | Accepted: `min(cpus, floor(memGB/2))` (s1.4) |
| 8 | leg concurrency unspecified | Accepted: serial (s1.4) |
| 9 | rsync across the Windows/WSL boundary | Accepted: measured in the plan; excludes added if slow (s1.4) |
| 10 | rsync carries Windows tools, `--delete` kills links | Accepted: tool locations excluded (s1.4) |
| 11 | buildx/login actions unpinned | Accepted: SHA-pinned (s1.6) |
| 12 | GHCR packages start private | Accepted: explicit rollout step (s5) |
| 13 | fork Dockerfile reaches a write token via a maintainer re-run | Review-first stays (s2). As of review fix round 1 (2026-10-17), only a non-pull-request run on `main` publishes; a pull request verifies locally and does not push |
| 14 | Dependabot read-only token | Noted: no Dependabot config today; same behavior as a fork PR if added (s2) |
| 15 | GHCR tags accumulate | Deferred with a note (s6) |
| - | build stops at the first failing step | Stated (s1.3) |
| - | GPU lane fragility | Verified first in s3.2; runtime dir for root in the image |
| - | image pull time | Stated (s2) |
| - | `image-tag.sh` on Windows | PowerShell twin with an equality test (s1.2) |
