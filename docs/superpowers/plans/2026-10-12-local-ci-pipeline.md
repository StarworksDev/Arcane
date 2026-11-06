# Local CI Pipeline (sub-project A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One repo-defined Ubuntu 24.04 build image, shared `scripts/ci/` build entry points, and one desk command (`scripts\ci\local.ps1`). The hosted Linux legs and the desk run the same sequence in the same image.

**Architecture:**
- `ci/linux/Dockerfile` bakes CI's toolchain into an image with a content-addressed tag, `build-<hash>`. `scripts/ci/image-tag.{sh,ps1}` compute the tag, and `ci-image` publishes it to `ghcr.io/starworksdev/arcane-ci`.
- `scripts/ci/build.{sh,ps1}` hold the build sequence that today sits inline in `ci.yml`.
- The hosted Linux legs run that sequence inside the image, pinned by digest.
- `scripts/ci/local.ps1` runs the same scripts: natively for Windows, and in the image for Linux. It mirrors the checkout into a volume for each leg.

**Tech Stack:**
- Docker (BuildKit/buildx, GHCR) and GitHub Actions `container:` jobs
- bash (3.2-safe) and POSIX sh
- Windows PowerShell 5.1 and pwsh 7
- premake5 gmake / MSBuild, Catch2 3.15 JUnit, rsync, ccache

**Spec:** `docs/superpowers/specs/2026-10-12-local-ci-pipeline-design.md` (rev 2, `cbb63f12`). Executors read both documents.

**Superseded in part (2026-10-17, review fix round 1).** Current policy is [docs/ci.md](../../ci.md). The second CI seed is `(run_number + 50021) mod 4294967296`. Only a non-pull-request run on `main` publishes the CI image. A pull request whose image inputs changed builds and verifies the image locally, does not push it, and skips the Linux legs that need the registry image. Script bodies, test pins, and workflow YAML below are the 2026-10-12 steps.

**Worktree / branch:** `D:/dev/starworks/Arcane-ci`, `ci/local-pipeline` (off `main` `946092fb`). Shell commands below are Git Bash unless marked PowerShell; `cd /d/dev/starworks/Arcane-ci` first.

## Global Constraints

**Image and pins:**
- Image: `ghcr.io/starworksdev/arcane-ci`. Tag `build-<first 16 hex of SHA-256>`; a local build is `arcane-ci:<tag>-local`.
- Base: `ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55`.
- apt snapshot: `APT_SNAPSHOT=20261012T000000Z` (snapshot.ubuntu.com is HTTPS-only; verified 2026-10-12).
- Pins are unchanged. They live in their scripts:
  - premake `5.0.0-beta8`
  - SDL3 `3.2.22`
  - DXC `v1.8.2505.1`
  - VVL `vulkan-sdk-1.4.328.0` @ `a1ff2dbc7e50828def8098c5ebf0fee10b714f85`
- The image's default user is **root**. User `arcane` is uid/gid 1000 and owns `/work` and `/ccache`. `local.ps1` runs containers with `--user 1000:1000`.

**Workflow rules (D26):**
- Every `uses:` in `ci.yml` is pinned to a 40-hex commit SHA.
- `packages: write` appears on the `ci-image` job only; the workflow default stays `permissions: contents: read`.
- No repository secret is referenced by a job that a pull request runs.
- Pinned actions (node24 releases; audit C3, 2026-10-12):
  - `actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1`, always with `persist-credentials: false`
  - `actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0`
  - `actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2`
  - `microsoft/setup-msbuild@30375c66a4eea26614e0d39710365f22f8b0af57 # v3`
  - `docker/login-action@dbcb813823bdd20940b903addbd779551569679f # v4.6.0`
  - `docker/setup-buildx-action@f87e5991a6d7451dcb8d9637bfbc97413f497069 # v4.4.1`
  - `StarworksDev/.github/actions/discord-ci-notify@af9284cd93ba45b7d5bd5818c9ee90812271535c # discord-ci-notify-v1.0.0`

**Script rules:**
- `scripts/ci/build.sh` is bash 3.2 + POSIX tools: no associative arrays, `${v,,}`, `mapfile`/`readarray`, or namerefs.
- `build.sh` invokes no `git`.
- Line endings:
  - `.sh` files are LF and committed `100755` (`git add --chmod=+x`).
  - `.ps1` and `.psm1` files are CRLF.
  - `ci/linux/Dockerfile` is LF (`.gitattributes`).
- PowerShell scripts run on Windows PowerShell 5.1 (the desk; no pwsh installed) and pwsh 7 (hosted runners): no `??`, no ternary, no `-Parallel`.

**Naming:**
- Leg names: `windows-msvc`, `linux-gcc-14`, `linux-clang-19`, `macos-appleclang`.
- Build logs: `logs/build-{generate,engine,referencegame,tests}.log`.
- Local output: `logs/local/<leg>-<Config>/`.

**Desk shell (Git Bash):**
- `export MSYS_NO_PATHCONV=1` once per session before any Task step. Git Bash otherwise rewrites container paths in arguments: `/v/verify-image.sh` becomes `C:/Program Files/Git/v/...`, and `-v a:/r:ro` is treated as a path list.
- Bind-mount sources are `"$(pwd -W)"` (`D:/dev/...`), never `"$PWD"` (`/d/dev/...`).
- `local.ps1` is unaffected: PowerShell does not convert paths.

**Project rules:**
- Pushing, opening a PR, and any GitHub-side change need the user's explicit go-ahead.
- Never force tooling: the existing entry points (`GenerateProjects.bat`, `run-tests.*`, the plain make sequence) keep working; `local.ps1` is optional.

## Resolutions (where this plan refines the spec)

| # | Spec text | Plan decision | Why |
|---|---|---|---|
| R1 | s1.4 "resolve the tag (`image-tag.sh` via Docker)" vs s1.2 "the desk computes the tag without starting a container" | `local.ps1` runs `image-tag.ps1` | s1.2 is the specific requirement; s3.1 proves the twins agree |
| R2 | s1.4 `docker build ... ci/linux` (context = `ci/linux`) | `docker build --build-context scripts=scripts -f ci/linux/Dockerfile ci/linux`, and the Dockerfile uses `COPY --from=scripts <file>...` | `COPY` cannot reach `scripts/` from a `ci/linux` context; a repo-root context would upload the whole checkout |
| R3 | s1.2 "input list ... in a fixed order" | The list lives in `ci/linux/image-inputs.txt`, read by both twins and hashed itself | One list, not two copies that drift |
| R4 | s1.2 "fails when the Dockerfile COPYs or RUNs a repo file the list does not contain" | Every repo file enters through `COPY --from=scripts` on one line. The checker refuses `ADD`, continued `COPY` lines, `COPY` from anything except `scripts` or a declared stage, and list entries the Dockerfile does not copy | A `RUN` can only reach repo files that a `COPY` brought in, so checking `COPY` covers `RUN` |
| R5 | s1.6 one `build-test` job with `container:` on the Linux legs | Separate jobs: `image-tag` (script tests + tag), `ci-image` (concurrency keyed on the tag), `build-test-linux` (container), `build-test` (Windows/macOS) | A job's `concurrency` cannot read its own step outputs. A matrix-wide `needs: ci-image` would also make Windows/macOS wait up to ~20 min on an image build |
| R6 | s1.6 legs pull `image@digest` | The Linux job passes `credentials:` (`github.actor` / `github.token`) and `permissions: packages: read` | The first run pulls the image before the package is made public (s5). `github.token` is the workflow's own token (read-only on fork PRs), not a repository secret |
| R7 | s1.1 "user `arcane` (uid/gid 1000)" | The Dockerfile first removes the base image's `ubuntu` user (uid 1000) | `ubuntu:24.04` ships `ubuntu` at uid 1000 |
| R8 | s1.1 apt `--snapshot` on every call | Bootstrap `ca-certificates` from the base image's archive, then set `APT::Snapshot` in `apt.conf.d`, and guard that the lists came from snapshot.ubuntu.com | The snapshot service is HTTPS-only, and the base image has no TLS roots |
| R9 | s1.4 rsync excludes | One explicit filter file, `scripts/ci/rsync-filter.txt`, not a `.gitignore` merge | `ReferenceProject/Saved/verify-layout.ini` is tracked but matches `.gitignore`'s `*.ini`; a gitignore filter would drop it |
| R10 | s1.4 "compiler launch through ccache" | The image has `/usr/lib/ccache/{gcc-14,g++-14,clang-19,clang++-19}` links; `local.ps1` puts `/usr/lib/ccache` first on `PATH` | Hosted CI keeps plain compilers (s6: no ccache in hosted CI) |
| R11 | s1.4 `-Seeds 2` "runs CI's pair" | Adds `-Seed N` (CI's N = the run number). The pair is `N`, `(N + 50021) mod 4294967296`. The default random N is < 42950 | s3.3 compares a local run with a specific CI run |
| R12 | s3.5 "every `uses:` is pinned" | The Discord notify action is pinned too (it was `@main`) | The acceptance check is literal |
| R13 | s1.1 / s3.2 "a writable runtime dir for root" + GPU-lane risk | `verify-image.sh` starts headless weston and Xvfb as root at image-build time | Surfaces the GPU-lane casualty before any leg runs |
| R14 | — | `vulkan-tools` added to the image | `verify-image.sh` uses `vulkaninfo` to prove lavapipe under Xvfb |
| R15 | s3.2 "the same ArcaneTests counts (cases / assertions)" | Parity compares the JUnit `tests` and `skipped` totals per `<leg>-<Config>[-gpu]`, and requires zero failures | JUnit carries no assertion count. The Catch2 console totals (cases + assertions) are still printed per leg by `local.ps1`'s summary and the CI logs, for a manual cross-check |

> **Audit amendments:** see the section of that name at the end. They change the action pins, the cache keys and the image package list, and add Tasks 13-14 and follow-ons F1-F3.

## Review Focus

1. **A desk worktree after a Windows build** (`.git` is a file pointing at `D:\...`; `bin/`, `*.vcxproj`, the Windows DXC are present). The Linux leg must neither copy those nor delete the leg's own `bin/`, Makefiles or tool links. Pinned in Task 8 Step 6.
2. **Ctrl-C during a Linux leg.** The container disappears (no leftover `arcane-local-*`), the volumes stay, and the next run is incremental. Pinned in Task 8 Step 8.
3. **Docker Desktop stopped, or `.wslconfig` still at 8 GB.** `local.ps1` names the fix and exits 2 before building anything; the low-limits case is a warning, not a stop. Pinned in Task 8 Step 7.
4. **A fork PR that changes the Dockerfile** (new tag). `ci-image` fails with the reason and never runs `buildx build`; a missing `SAME_REPO` counts as a fork. Pinned in Task 7.
5. **The package not pullable yet** (before the first publish, or still private). `local.ps1` falls back to a local build and the summary labels those legs `<tag>-local (local image)`. Pinned in Task 8 Step 5.

---

### Task 1: `ArcaneCi.psm1` — the PowerShell helpers, test-first

**Files:**
- Create: `scripts/ci/ArcaneCi.psm1`
- Create: `scripts/ci/compare-test-counts.ps1`
- Test: `scripts/ci/tests/test-arcane-ci.ps1`

**Interfaces:**
- Consumes: nothing.
- Produces (later tasks call these exact names):
  - `Resolve-LocalRuns [-Leg string[]] [-Config string[]] [-Full]` → array of `[pscustomobject]{Leg, Config, Kind ('windows'|'linux'), CC, CXX}`, canonical order
  - `Get-SeedList -Seed string [-Seeds 1|2]` → string[]
  - `Get-LinuxJobCount -Cpus int -MemoryBytes long` → int
  - `ConvertTo-Bytes string` → long
  - `Set-WslConfigLimits -Text string [-Memory '24GB'] [-Processors 16]` → string
  - `Test-WslConfigLimits -Text string [-MinMemoryBytes 24GB] [-MinProcessors 16]` → bool
  - `Get-Catch2Totals -Path` → `{Found, Cases, CasesPassed, CasesFailed, CasesSkipped, Assertions, AssertionsFailed}`
  - `Get-JUnitTotals -Path` → `{Tests, Failures, Errors, Skipped}`
  - `Compare-JUnitDirs -Baseline dir -Candidate dir [-Key pattern[]]` → rows `{Key, BaselineTests, CandidateTests, BaselineSkipped, CandidateSkipped, CandidateFailures, Match}`
  - `Get-BuildErrorLines [-Path string[]] [-Max 40]` → string[]
  - `Read-LegStatus -Path` → OrderedDictionary
  - `Write-LegStatus -Path -Status IDictionary`
  - `New-LegResult -Run -Status -OutDir [-Seeds] [-Seconds] [-Image]` → `{Leg, Config, Image, Build, Tests, Seconds, OutDir, Passed}`
  - `Format-LocalSummary -Results object[]` → string[]
  - `Invoke-NativeLogged -FilePath [-ArgumentList] -LogPath` → int exit code (tees combined output to a UTF-8-no-BOM log and the host)
  - `Invoke-NativeQuiet -FilePath [-ArgumentList]` → `{Code, Output}` (stderr dropped)
  - `Resolve-MSBuild` → MSBuild.exe path (PATH first, then vswhere), throws when absent

- [ ] **Step 1: Write the failing test**

Create `scripts/ci/tests/test-arcane-ci.ps1`:

```powershell
# test-arcane-ci.ps1 -- unit tests for scripts\ci\ArcaneCi.psm1. No Pester:
# plain checks, so it runs unchanged on Windows PowerShell 5.1 (the desk) and
# pwsh 7 (the image-tag job in .github/workflows/ci.yml). Exit 0 = all pass.
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'ArcaneCi.psm1') -Force

$script:Failed = 0
function Check([string]$Name, [bool]$Condition, [string]$Detail = '') {
    if ($Condition) { Write-Host "ok   $Name" }
    else { Write-Host "FAIL $Name $Detail" -ForegroundColor Red; $script:Failed++ }
}
function Throws([scriptblock]$Block) {
    try { & $Block | Out-Null; return $false } catch { return $true }
}
function RunsText($Runs) { ($Runs | ForEach-Object { "$($_.Leg)/$($_.Config)" }) -join ',' }
function Put([string]$Path, [string]$Text) {
    $dir = Split-Path -Parent $Path
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text)
}
function JUnit([int]$Tests, [int]$Skipped, [int]$Failures) {
    "<?xml version=`"1.0`" encoding=`"UTF-8`"?><testsuites><testsuite name=`"ArcaneTests`" errors=`"0`" failures=`"$Failures`" skipped=`"$Skipped`" tests=`"$Tests`"/></testsuites>"
}

# Fixture paths use '/': pwsh on Linux hands '\' to .NET as a filename character.
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('arcane-ci-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    # --- Resolve-LocalRuns ---------------------------------------------------
    Check 'runs: default' ((RunsText (Resolve-LocalRuns)) -eq 'windows-msvc/Debug,linux-gcc-14/Debug')
    Check 'runs: -Full' ((RunsText (Resolve-LocalRuns -Full)) -eq 'windows-msvc/Debug,windows-msvc/Release,windows-msvc/Dist,linux-gcc-14/Debug,linux-gcc-14/Release,linux-gcc-14/Dist,linux-clang-19/Debug,linux-clang-19/Release,linux-clang-19/Dist')
    Check 'runs: one leg, one config' ((RunsText (Resolve-LocalRuns -Leg linux-clang-19 -Config Release)) -eq 'linux-clang-19/Release')
    Check 'runs: leg only -> Debug' ((RunsText (Resolve-LocalRuns -Leg linux-clang-19)) -eq 'linux-clang-19/Debug')
    Check 'runs: config only -> default legs' ((RunsText (Resolve-LocalRuns -Config Release)) -eq 'windows-msvc/Release,linux-gcc-14/Release')
    Check 'runs: -Full narrowed by -Leg' ((RunsText (Resolve-LocalRuns -Full -Leg linux-gcc-14)) -eq 'linux-gcc-14/Debug,linux-gcc-14/Release,linux-gcc-14/Dist')
    Check 'runs: canonical order' ((RunsText (Resolve-LocalRuns -Leg linux-clang-19, windows-msvc -Config Dist, Debug)) -eq 'windows-msvc/Debug,windows-msvc/Dist,linux-clang-19/Debug,linux-clang-19/Dist')
    Check 'runs: a linux leg carries its compilers' ((Resolve-LocalRuns -Leg linux-clang-19)[0].CXX -eq 'clang++-19')
    Check 'runs: windows leg kind' ((Resolve-LocalRuns -Leg windows-msvc)[0].Kind -eq 'windows')
    Check 'runs: macOS refused' (Throws { Resolve-LocalRuns -Leg macos-appleclang })
    Check 'runs: unknown leg refused' (Throws { Resolve-LocalRuns -Leg linux-gcc-13 })
    Check 'runs: unknown config refused' (Throws { Resolve-LocalRuns -Config Profile })

    # --- Get-SeedList --------------------------------------------------------
    Check 'seeds: one' ((((Get-SeedList -Seed '123' -Seeds 1) -join ',')) -eq '123')
    Check 'seeds: the CI pair' ((((Get-SeedList -Seed '123' -Seeds 2) -join ',')) -eq '123,12350021')
    Check 'seeds: 42949 pair still fits uint32' ((((Get-SeedList -Seed '42949' -Seeds 2) -join ',')) -eq '42949,4294950021')
    Check 'seeds: non-numeric refused' (Throws { Get-SeedList -Seed 'abc' })
    Check 'seeds: pair overflowing uint32 refused' (Throws { Get-SeedList -Seed '99999' -Seeds 2 })

    # --- Get-LinuxJobCount ---------------------------------------------------
    Check 'jobs: 16 cpu / 24 GiB -> 12' ((Get-LinuxJobCount -Cpus 16 -MemoryBytes 24GB) -eq 12)
    Check 'jobs: 16 cpu / 8 GiB -> 4' ((Get-LinuxJobCount -Cpus 16 -MemoryBytes 8GB) -eq 4)
    Check 'jobs: 4 cpu / 64 GiB -> 4' ((Get-LinuxJobCount -Cpus 4 -MemoryBytes 64GB) -eq 4)
    Check 'jobs: never below 1' ((Get-LinuxJobCount -Cpus 16 -MemoryBytes 1GB) -eq 1)
    Check 'jobs: 23.3 GiB rounds to 23 -> 11' ((Get-LinuxJobCount -Cpus 16 -MemoryBytes 25001000000) -eq 11)

    # --- ConvertTo-Bytes -----------------------------------------------------
    Check 'bytes: 24GB' ((ConvertTo-Bytes '24GB') -eq 25769803776)
    Check 'bytes: plain number' ((ConvertTo-Bytes '8589934592') -eq 8589934592)
    Check 'bytes: 512mb' ((ConvertTo-Bytes '512mb') -eq 536870912)
    Check 'bytes: junk refused' (Throws { ConvertTo-Bytes 'lots' })

    # --- Set-WslConfigLimits / Test-WslConfigLimits --------------------------
    Check 'wsl: empty file' ((Set-WslConfigLimits -Text '') -ceq "[wsl2]`r`nmemory=24GB`r`nprocessors=16`r`n")
    $desk = "[wsl2]`nmemory=8589934592`nprocessors=10"
    Check 'wsl: the desk file today' ((Set-WslConfigLimits -Text $desk) -ceq "[wsl2]`nmemory=24GB`nprocessors=16")
    $other = "[wsl2]`nswap=0`n`n[experimental]`nautoMemoryReclaim=gradual`n"
    $otherOut = Set-WslConfigLimits -Text $other
    Check 'wsl: other keys and sections kept' ($otherOut -ceq "[wsl2]`nmemory=24GB`nprocessors=16`nswap=0`n`n[experimental]`nautoMemoryReclaim=gradual`n")
    Check 'wsl: idempotent' ((Set-WslConfigLimits -Text $otherOut) -ceq $otherOut)
    Check 'wsl: section appended' ((Set-WslConfigLimits -Text "[experimental]`r`nsparseVhd=true`r`n") -ceq "[experimental]`r`nsparseVhd=true`r`n`r`n[wsl2]`r`nmemory=24GB`r`nprocessors=16`r`n")
    Check 'wsl: case-insensitive keys' ((Set-WslConfigLimits -Text "[WSL2]`nMemory = 4GB`n") -ceq "[WSL2]`nmemory=24GB`nprocessors=16`n")
    Check 'wsl: missing key lands after its predecessor' ((Set-WslConfigLimits -Text "[wsl2]`nswap=0`nmemory=8GB`nlocalhostForwarding=true`n") -ceq "[wsl2]`nswap=0`nmemory=24GB`nprocessors=16`nlocalhostForwarding=true`n")
    Check 'wsl: only processors present' ((Set-WslConfigLimits -Text "[wsl2]`nprocessors=4`n") -ceq "[wsl2]`nmemory=24GB`nprocessors=16`n")
    Check 'wsl limits: desk today is below' (-not (Test-WslConfigLimits -Text $desk))
    Check 'wsl limits: after Set' (Test-WslConfigLimits -Text (Set-WslConfigLimits -Text $desk))
    Check 'wsl limits: missing processors' (-not (Test-WslConfigLimits -Text "[wsl2]`nmemory=24GB`n"))
    Check 'wsl limits: empty' (-not (Test-WslConfigLimits -Text ''))
    Check 'wsl limits: above' (Test-WslConfigLimits -Text "[wsl2]`nmemory=32GB`nprocessors=20")
    Check 'wsl limits: other section ignored' (-not (Test-WslConfigLimits -Text "[experimental]`nmemory=32GB`nprocessors=20"))

    # --- Get-Catch2Totals ----------------------------------------------------
    $p = Join-Path $tmp 'c1.log'; Put $p "noise`nAll tests passed (1769 assertions in 204 test cases)`n"
    $t = Get-Catch2Totals -Path $p
    Check 'catch2: all passed' ($t.Found -and $t.Cases -eq 204 -and $t.CasesPassed -eq 204 -and $t.CasesFailed -eq 0 -and $t.Assertions -eq 1769)
    $p = Join-Path $tmp 'c2.log'; Put $p "====`r`ntest cases:  201 |  199 passed | 2 skipped`r`nassertions: 1717 | 1717 passed`r`n"
    $t = Get-Catch2Totals -Path $p
    Check 'catch2: skipped' ($t.Found -and $t.Cases -eq 201 -and $t.CasesPassed -eq 199 -and $t.CasesSkipped -eq 2 -and $t.CasesFailed -eq 0 -and $t.Assertions -eq 1717)
    $p = Join-Path $tmp 'c3.log'; Put $p "test cases:   10 |    8 passed | 1 failed | 1 skipped`nassertions: 1000 |  998 passed | 2 failed`n"
    $t = Get-Catch2Totals -Path $p
    Check 'catch2: failed' ($t.Cases -eq 10 -and $t.CasesPassed -eq 8 -and $t.CasesFailed -eq 1 -and $t.CasesSkipped -eq 1 -and $t.Assertions -eq 1000 -and $t.AssertionsFailed -eq 2)
    $p = Join-Path $tmp 'c4.log'; Put $p "test cases: 5 | 4 passed | 1 failed as expected`nassertions: 9 | 9 passed`n"
    Check 'catch2: failed-as-expected is not a failure' ((Get-Catch2Totals -Path $p).CasesFailed -eq 0)
    $p = Join-Path $tmp 'c5.log'; Put $p "the build died before the suite`n"
    Check 'catch2: no summary' (-not (Get-Catch2Totals -Path $p).Found)
    Check 'catch2: missing file' (-not (Get-Catch2Totals -Path (Join-Path $tmp 'nope.log')).Found)

    # --- JUnit totals + Compare-JUnitDirs ------------------------------------
    Put (Join-Path $tmp 'base/test-results-linux-gcc-14-Debug/linux-gcc-14-Debug-seed1.junit.xml') (JUnit 100 2 0)
    Put (Join-Path $tmp 'base/test-results-linux-gcc-14-Debug/linux-gcc-14-Debug-seed150021.junit.xml') (JUnit 100 2 0)
    Put (Join-Path $tmp 'base/test-results-windows-msvc-Debug/windows-msvc-Debug-seed1.junit.xml') (JUnit 50 0 0)
    Put (Join-Path $tmp 'cand/x/linux-gcc-14-Debug-seed9.junit.xml') (JUnit 100 2 0)
    Put (Join-Path $tmp 'cand/x/windows-msvc-Debug-seed9.junit.xml') (JUnit 49 0 0)
    Put (Join-Path $tmp 'cand/x/linux-clang-19-Release-seed9.junit.xml') (JUnit 80 0 0)
    Put (Join-Path $tmp 'cand/x/linux-gcc-14-Debug-gpu-seed9.junit.xml') (JUnit 30 0 1)
    Put (Join-Path $tmp 'base/x/linux-gcc-14-Debug-gpu-seed1.junit.xml') (JUnit 30 0 0)
    $jt = Get-JUnitTotals -Path (Join-Path $tmp 'cand/x/linux-gcc-14-Debug-gpu-seed9.junit.xml')
    Check 'junit: totals' ($jt.Tests -eq 30 -and $jt.Failures -eq 1 -and $jt.Skipped -eq 0)
    $rows = Compare-JUnitDirs -Baseline (Join-Path $tmp 'base') -Candidate (Join-Path $tmp 'cand')
    $byKey = @{}; foreach ($r in $rows) { $byKey[$r.Key] = $r }
    $one = Compare-JUnitDirs -Baseline (Join-Path $tmp 'base') -Candidate (Join-Path $tmp 'cand') -Key 'linux-gcc-14-Debug'
    Check 'compare: -Key keeps only the named key (no -gpu row)' ($one.Count -eq 1 -and $one[0].Key -eq 'linux-gcc-14-Debug' -and $one[0].Match)
    Check 'compare: four keys' ($rows.Count -eq 4)
    Check 'compare: matching key' ($byKey['linux-gcc-14-Debug'].Match)
    Check 'compare: count drift' (-not $byKey['windows-msvc-Debug'].Match)
    Check 'compare: missing baseline' (-not $byKey['linux-clang-19-Release'].Match)
    Check 'compare: candidate failure' (-not $byKey['linux-gcc-14-Debug-gpu'].Match -and $byKey['linux-gcc-14-Debug-gpu'].CandidateFailures -eq 1)

    # --- Get-BuildErrorLines ---------------------------------------------------
    $p = Join-Path $tmp 'logs/build-engine.log'
    Put $p "ok line`nfoo.cpp:1:2: error: bad`nfoo.cpp:1:2: error: bad`n/usr/bin/ld: x.o: undefined reference to ``y'`nwarning: meh`nError: capitalised is not matched`n"
    $errs = Get-BuildErrorLines -Path @($p, (Join-Path $tmp 'missing.log'))
    Check 'errors: unique, case-sensitive' ($errs.Count -eq 2 -and ($errs -contains 'foo.cpp:1:2: error: bad'))
    Check 'errors: no logs' ((Get-BuildErrorLines -Path @()).Count -eq 0)

    # --- leg status ------------------------------------------------------------
    $sp = Join-Path $tmp 'status.txt'
    Write-LegStatus -Path $sp -Status ([ordered]@{ build = 0; 'tests-7' = 1 })
    $s = Read-LegStatus -Path $sp
    Check 'status: round trip' ($s['build'] -eq 0 -and $s['tests-7'] -eq 1 -and $s.Count -eq 2)
    Put $sp "sync_seconds=12`nnot a pair`nbuild=2`n"
    $s = Read-LegStatus -Path $sp
    Check 'status: junk ignored' ($s['build'] -eq 2 -and $s['sync_seconds'] -eq 12 -and $s.Count -eq 2)
    Check 'status: missing file' ((Read-LegStatus -Path (Join-Path $tmp 'none.txt')).Count -eq 0)

    # --- New-LegResult + Format-LocalSummary -----------------------------------
    $leg = Join-Path $tmp 'leg'
    Put (Join-Path $leg 'tests-seed7.log') "All tests passed (10 assertions in 2 test cases)`n"
    Put (Join-Path $leg 'build-engine.log') "a.cpp:1: error: boom`n"
    $run = (Resolve-LocalRuns -Leg linux-gcc-14)[0]
    $ok = New-LegResult -Run $run -Status ([ordered]@{ build = 0; 'tests-7' = 0 }) -OutDir $leg -Seeds @('7') -Seconds 90 -Image 'build-x'
    Check 'result: pass' ($ok.Passed -and $ok.Build -eq 'ok' -and $ok.Tests -like 'seed 7: 2/2 cases*10 assertions*')
    $red = New-LegResult -Run $run -Status ([ordered]@{ build = 2 }) -OutDir $leg -Seeds @('7') -Seconds 30 -Image 'build-x'
    Check 'result: build failure' ((-not $red.Passed) -and $red.Build -eq 'FAILED (2)')
    $oom = New-LegResult -Run $run -Status ([ordered]@{ build = 2; oom = 1 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: OOM named' ((-not $oom.Passed) -and $oom.Build -like 'OOM*-Jobs*')
    $killed = New-LegResult -Run $run -Status ([ordered]@{ container = 137 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: a killed container is not called OOM' ((-not $killed.Passed) -and $killed.Build -notlike 'OOM*')
    $none = New-LegResult -Run $run -Status ([ordered]@{ build = 0 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: tests that never ran fail the leg' ((-not $none.Passed) -and $none.Tests -eq 'tests did not run')
    $tf = New-LegResult -Run $run -Status ([ordered]@{ build = 0; 'tests-7' = 1 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: test failure' ((-not $tf.Passed) -and $tf.Tests -like 'seed 7: FAILED (1)*')
    $ce = New-LegResult -Run $run -Status ([ordered]@{ container = 125 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: container never built' ((-not $ce.Passed) -and $ce.Build -like 'not run (container exit 125*')
    $text = (Format-LocalSummary -Results @($ok, $red)) -join "`n"
    Check 'summary: verdicts' ($text -match 'PASS' -and $text -match 'FAIL')
    Check 'summary: first build errors shown' ($text -match 'a\.cpp:1: error: boom')
    Check 'summary: count line' ($text -match '1 of 2 leg\(s\) passed')

    # --- native process helpers ------------------------------------------------
    $ps = (Get-Process -Id $PID).Path
    $log = Join-Path $tmp 'sub/native.log'
    $code = Invoke-NativeLogged -FilePath $ps -ArgumentList @('-NoProfile', '-Command', "Write-Output 'out-line'; [Console]::Error.WriteLine('err-line'); exit 3") -LogPath $log
    $bytes = [System.IO.File]::ReadAllBytes($log)
    $logText = [System.IO.File]::ReadAllText($log)
    Check 'native: exit code' ($code -eq 3)
    Check 'native: stdout and stderr logged' ($logText -match 'out-line' -and $logText -match 'err-line')
    Check 'native: UTF-8 without BOM' (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF))
    $q = Invoke-NativeQuiet -FilePath $ps -ArgumentList @('-NoProfile', '-Command', "Write-Output 'q'; exit 4")
    Check 'quiet: exit code and stdout' ($q.Code -eq 4 -and ($q.Output -join '') -eq 'q')
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($script:Failed -gt 0) { Write-Host "$($script:Failed) check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host 'all checks passed'
exit 0
```

- [ ] **Step 2: Run it and watch it fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci/tests/test-arcane-ci.ps1`
Expected: a terminating error from `Import-Module` (`ArcaneCi.psm1` was not found), exit non-zero.

- [ ] **Step 3: Write the module**

Create `scripts/ci/ArcaneCi.psm1`:

```powershell
# ArcaneCi.psm1 -- helpers shared by scripts\ci\build.ps1, local.ps1,
# setup-desk.ps1 and compare-test-counts.ps1 (docs/ci.md, "Local runs").
# Pure functions first (unit-tested by scripts\ci\tests\test-arcane-ci.ps1 on
# Windows PowerShell 5.1 and pwsh 7), then the ones that start processes.
# Windows PowerShell 5.1 compatible: no ternary, no ??, no -Parallel.
Set-StrictMode -Version 2.0

$script:LegTable = [ordered]@{
    'windows-msvc'   = @{ Kind = 'windows'; CC = '';         CXX = '' }
    'linux-gcc-14'   = @{ Kind = 'linux';   CC = 'gcc-14';   CXX = 'g++-14' }
    'linux-clang-19' = @{ Kind = 'linux';   CC = 'clang-19'; CXX = 'clang++-19' }
}
$script:AllConfigs = @('Debug', 'Release', 'Dist')

function Get-FullPath([string]$Path) {
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

# The (leg, config) pairs a local run builds, in canonical order (legs in
# LegTable order, configs Debug/Release/Dist). -Full selects every local leg
# and config; an explicit -Leg / -Config narrows its own dimension.
function Resolve-LocalRuns {
    param([string[]]$Leg, [string[]]$Config, [switch]$Full)
    $legs = @('windows-msvc', 'linux-gcc-14')
    if ($Leg) { $legs = $Leg } elseif ($Full) { $legs = @($script:LegTable.Keys) }
    $configs = @('Debug')
    if ($Config) { $configs = $Config } elseif ($Full) { $configs = $script:AllConfigs }
    foreach ($l in $legs) {
        if (-not $script:LegTable.Contains($l)) {
            if ($l -eq 'macos-appleclang') { throw 'local: macos-appleclang runs on the hosted macOS runner only' }
            throw "local: unknown leg '$l' (windows-msvc, linux-gcc-14, linux-clang-19)"
        }
    }
    foreach ($c in $configs) {
        if ($script:AllConfigs -notcontains $c) { throw "local: unknown config '$c' (Debug, Release, Dist)" }
    }
    $runs = @()
    foreach ($l in $script:LegTable.Keys) {
        if ($legs -notcontains $l) { continue }
        foreach ($c in $script:AllConfigs) {
            if ($configs -notcontains $c) { continue }
            $e = $script:LegTable[$l]
            $runs += [pscustomobject]@{ Leg = $l; Config = $c; Kind = $e.Kind; CC = $e.CC; CXX = $e.CXX }
        }
    }
    return ,$runs
}

# One seed, or CI's pair: SEED_A = N, SEED_B = (N + 50021) mod 4294967296
# (docs/ci.md). The body below is the 2026-10-12 concatenation step.
function Get-SeedList {
    param([Parameter(Mandatory)][string]$Seed, [ValidateSet(1, 2)][int]$Seeds = 1)
    if ($Seed -notmatch '^[0-9]+$') { throw "local: -Seed must be a non-negative integer (got '$Seed')" }
    $list = @($Seed)
    if ($Seeds -eq 2) { $list += "${Seed}50021" }
    foreach ($s in $list) {
        if ([decimal]$s -gt 4294967295) { throw "local: seed $s does not fit Catch2's 32-bit --rng-seed (with -Seeds 2 keep -Seed below 42950)" }
    }
    return ,$list
}

# A Linux leg's make -j: min(CPUs, memory GB / 2) of what Docker reports --
# a Debug link peaks near 2 GB per job.
function Get-LinuxJobCount {
    param([Parameter(Mandatory)][int]$Cpus, [Parameter(Mandatory)][long]$MemoryBytes)
    $memGB = [math]::Round($MemoryBytes / 1GB)
    $jobs = [math]::Min($Cpus, [math]::Floor($memGB / 2))
    return [int][math]::Max(1, $jobs)
}

function ConvertTo-Bytes {
    param([Parameter(Mandatory)][string]$Value)
    $m = [regex]::Match($Value.Trim(), '^(\d+)\s*([KMGT]?B)?$', 'IgnoreCase')
    if (-not $m.Success) { throw "not a size: '$Value'" }
    $n = [long]$m.Groups[1].Value
    switch ($m.Groups[2].Value.ToUpperInvariant()) {
        'KB' { return $n * 1KB }
        'MB' { return $n * 1MB }
        'GB' { return $n * 1GB }
        'TB' { return $n * 1TB }
        default { return $n }
    }
}

# Sets [wsl2] memory= and processors= in a .wslconfig's text, keeping every
# other key, section, the file's newline style and its trailing newline.
function Set-WslConfigLimits {
    param([AllowEmptyString()][string]$Text, [string]$Memory = '24GB', [int]$Processors = 16)
    if ($null -eq $Text) { $Text = '' }
    $nl = "`r`n"
    if ($Text.Contains("`r`n")) { $nl = "`r`n" } elseif ($Text.Contains("`n")) { $nl = "`n" }
    $trailing = ($Text.Length -eq 0) -or $Text.EndsWith("`n")
    $lines = New-Object System.Collections.Generic.List[string]
    if ($Text.Length -gt 0) {
        $body = $Text
        if ($body.EndsWith("`n")) { $body = $body.Substring(0, $body.Length - 1) }
        if ($body.EndsWith("`r")) { $body = $body.Substring(0, $body.Length - 1) }
        foreach ($l in ($body -split "`r?`n")) { $lines.Add($l) }
    }
    $want = [ordered]@{ memory = "memory=$Memory"; processors = "processors=$Processors" }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -match '^\[wsl2\]$') { $start = $i; break }
    }
    if ($start -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -ne '') { $lines.Add('') }
        $lines.Add('[wsl2]')
        foreach ($v in $want.Values) { $lines.Add($v) }
    } else {
        $end = $lines.Count
        for ($i = $start + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i].Trim().StartsWith('[')) { $end = $i; break }
        }
        # $at[key] = the line index of that key inside the section.
        $at = @{}
        for ($i = $start + 1; $i -lt $end; $i++) {
            foreach ($k in @($want.Keys)) {
                if ($lines[$i] -match "^\s*$k\s*=") { $lines[$i] = $want[$k]; $at[$k] = $i }
            }
        }
        # A missing key goes right after the previous wanted key's line (so
        # memory= stays above processors=), else directly under the header.
        $insert = $start + 1
        foreach ($k in @($want.Keys)) {
            if ($at.ContainsKey($k)) { $insert = $at[$k] + 1; continue }
            $lines.Insert($insert, $want[$k])
            foreach ($other in @($at.Keys)) { if ($at[$other] -ge $insert) { $at[$other]++ } }
            $at[$k] = $insert
            $insert++
        }
    }
    $out = [string]::Join($nl, $lines.ToArray())
    if ($trailing) { $out += $nl }
    return $out
}

function Test-WslConfigLimits {
    param([AllowEmptyString()][string]$Text, [long]$MinMemoryBytes = 24GB, [int]$MinProcessors = 16)
    if (-not $Text) { return $false }
    $in = $false; $mem = $null; $cpu = $null
    foreach ($l in ($Text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t.StartsWith('[')) { $in = ($t -match '^\[wsl2\]$'); continue }
        if (-not $in) { continue }
        if ($t -match '^memory\s*=\s*(.+)$') {
            try { $mem = ConvertTo-Bytes $Matches[1] } catch { $mem = $null }
        } elseif ($t -match '^processors\s*=\s*(\d+)$') {
            $cpu = [int]$Matches[1]
        }
    }
    return (($null -ne $mem) -and ($null -ne $cpu) -and ($mem -ge $MinMemoryBytes) -and ($cpu -ge $MinProcessors))
}

function Get-Catch2Segment([string]$Text, [string]$Word) {
    $m = [regex]::Match($Text, "(\d+) $Word(?! as)")
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return 0
}

# The suite totals from a Catch2 console log: "All tests passed (A assertions
# in C test cases)", or the "test cases: | assertions:" block -- whichever
# comes last in the log.
function Get-Catch2Totals {
    param([Parameter(Mandatory)][string]$Path)
    $r = [pscustomobject]@{ Found = $false; Cases = 0; CasesPassed = 0; CasesFailed = 0; CasesSkipped = 0; Assertions = 0; AssertionsFailed = 0 }
    if (-not (Test-Path -LiteralPath $Path)) { return $r }
    $text = [System.IO.File]::ReadAllText((Get-FullPath $Path))
    $all = [regex]::Matches($text, 'All tests passed \((\d+) assertions? in (\d+) test cases?\)')
    $cases = [regex]::Matches($text, '(?m)^test cases:\s*(\d+)\s*\|([^\r\n]*)')
    $asserts = [regex]::Matches($text, '(?m)^assertions:\s*(\d+)\s*\|([^\r\n]*)')
    $a = $null; if ($all.Count -gt 0) { $a = $all[$all.Count - 1] }
    $c = $null; if ($cases.Count -gt 0) { $c = $cases[$cases.Count - 1] }
    if ($a -and ((-not $c) -or $a.Index -gt $c.Index)) {
        $r.Found = $true
        $r.Assertions = [int]$a.Groups[1].Value
        $r.Cases = [int]$a.Groups[2].Value
        $r.CasesPassed = $r.Cases
    } elseif ($c) {
        $r.Found = $true
        $r.Cases = [int]$c.Groups[1].Value
        $r.CasesPassed = Get-Catch2Segment $c.Groups[2].Value 'passed'
        $r.CasesFailed = Get-Catch2Segment $c.Groups[2].Value 'failed'
        $r.CasesSkipped = Get-Catch2Segment $c.Groups[2].Value 'skipped'
        if ($asserts.Count -gt 0) {
            $x = $asserts[$asserts.Count - 1]
            $r.Assertions = [int]$x.Groups[1].Value
            $r.AssertionsFailed = Get-Catch2Segment $x.Groups[2].Value 'failed'
        }
    }
    return $r
}

function Get-JUnitTotals {
    param([Parameter(Mandatory)][string]$Path)
    [xml]$x = [System.IO.File]::ReadAllText((Get-FullPath $Path))
    $t = [pscustomobject]@{ Tests = 0; Failures = 0; Errors = 0; Skipped = 0 }
    foreach ($s in @($x.SelectNodes('//testsuite'))) {
        foreach ($k in @('tests', 'failures', 'errors', 'skipped')) {
            $v = $s.GetAttribute($k)
            if ($v) { $t.$k += [int]$v }
        }
    }
    return $t
}

# Compares two downloaded sets of CI test-results artifacts, keyed
# <leg>-<Config>[-gpu] (the JUnit file name without its seed). A key matches
# when both sides have it, the tests/skipped counts agree (across every seed
# file of the candidate), and the candidate has no failures or errors.
# -Key limits the comparison to keys matching any of the given -like patterns
# (e.g. a local run has no -gpu lane: -Key linux-gcc-14-Debug).
function Compare-JUnitDirs {
    param([Parameter(Mandatory)][string]$Baseline, [Parameter(Mandatory)][string]$Candidate, [string[]]$Key = @())
    $sets = @{}
    foreach ($side in @('Baseline', 'Candidate')) {
        $dir = $Baseline; if ($side -eq 'Candidate') { $dir = $Candidate }
        $map = @{}
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -Recurse -Filter '*.junit.xml')) {
            # $name, not $key: PowerShell names are case-insensitive, and $Key is the
            # [string[]] parameter -- assigning to it would coerce to an array.
            $name = $f.Name -replace '-seed[^.]*\.junit\.xml$', ''
            if (-not $map.ContainsKey($name)) { $map[$name] = [System.Collections.Generic.List[object]]::new() }
            $map[$name].Add((Get-JUnitTotals -Path $f.FullName))
        }
        $sets[$side] = $map
    }
    $keys = @(@($sets['Baseline'].Keys) + @($sets['Candidate'].Keys) | Sort-Object -Unique)
    if ($Key.Count -gt 0) {
        $keys = @($keys | Where-Object { $k = $_; @($Key | Where-Object { $k -like $_ }).Count -gt 0 })
    }
    $rows = @()
    foreach ($k in $keys) {
        $b = $sets['Baseline'][$k]; $c = $sets['Candidate'][$k]
        $row = [pscustomobject]@{ Key = $k; BaselineTests = $null; CandidateTests = $null; BaselineSkipped = $null; CandidateSkipped = $null; CandidateFailures = $null; Match = $false }
        if ($b) { $row.BaselineTests = $b[0].Tests; $row.BaselineSkipped = $b[0].Skipped }
        if ($c) {
            $row.CandidateTests = $c[0].Tests; $row.CandidateSkipped = $c[0].Skipped
            $fails = 0; $consistent = $true
            foreach ($one in $c) {
                $fails += $one.Failures + $one.Errors
                if ($one.Tests -ne $c[0].Tests -or $one.Skipped -ne $c[0].Skipped) { $consistent = $false }
            }
            $row.CandidateFailures = $fails
            $row.Match = [bool]($b -and $consistent -and $fails -eq 0 -and $b[0].Tests -eq $c[0].Tests -and $b[0].Skipped -eq $c[0].Skipped)
        }
        $rows += $row
    }
    return ,$rows
}

# The distinct compiler/linker error lines of build logs (the grep ci.yml's
# "Summarize build errors" step uses; case-sensitive like grep -E).
function Get-BuildErrorLines {
    param([string[]]$Path = @(), [int]$Max = 40)
    $files = @($Path | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
    if ($files.Count -eq 0) { return ,@() }
    $hits = @(Select-String -LiteralPath $files -Pattern '(error|undefined reference|Undefined symbols|ld: )' -CaseSensitive |
        ForEach-Object { $_.Line.Trim() })
    return ,@($hits | Sort-Object -Unique | Select-Object -First $Max)
}

# status.txt: one key=integer per line (build=, tests-<seed>=, sync=, ...).
function Read-LegStatus {
    param([Parameter(Mandatory)][string]$Path)
    $s = [ordered]@{}
    if (Test-Path -LiteralPath $Path) {
        foreach ($l in [System.IO.File]::ReadAllLines((Get-FullPath $Path))) {
            if ($l -match '^([A-Za-z0-9_.-]+)=(-?\d+)$') { $s[$Matches[1]] = [int]$Matches[2] }
        }
    }
    return $s
}

function Write-LegStatus {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][System.Collections.IDictionary]$Status)
    $lines = @(foreach ($k in $Status.Keys) { "$k=$($Status[$k])" })
    [System.IO.File]::WriteAllLines((Get-FullPath $Path), [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

# One summary row for a leg. A leg passes only when it built AND at least one
# seed ran AND every seed passed.
function New-LegResult {
    param(
        [Parameter(Mandatory)]$Run,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Status,
        [Parameter(Mandatory)][string]$OutDir,
        [string[]]$Seeds = @(),
        [double]$Seconds = 0,
        [string]$Image = ''
    )
    $passed = $false
    if ($Status.Contains('oom')) {
        $build = "OOM (a compiler or linker was killed; build exit $($Status['build'])): lower -Jobs"
    } elseif ($Status.Contains('build')) {
        if ([int]$Status['build'] -eq 0) { $build = 'ok'; $passed = $true } else { $build = "FAILED ($($Status['build']))" }
    } elseif ($Status.Contains('container')) {
        $build = "not run (container exit $($Status['container']); see container.log)"
    } else {
        $build = 'not run'
    }
    $tests = New-Object System.Collections.Generic.List[string]
    foreach ($s in $Seeds) {
        $key = "tests-$s"
        if (-not $Status.Contains($key)) { continue }
        $t = Get-Catch2Totals -Path (Join-Path $OutDir "tests-seed$s.log")
        if ($t.Found) {
            $parts = New-Object System.Collections.Generic.List[string]
            $parts.Add("$($t.CasesPassed)/$($t.Cases) cases")
            if ($t.CasesFailed -gt 0) { $parts.Add("$($t.CasesFailed) failed") }
            if ($t.CasesSkipped -gt 0) { $parts.Add("$($t.CasesSkipped) skipped") }
            $parts.Add("$($t.Assertions) assertions")
            $desc = $parts -join ', '
        } else {
            $desc = 'no Catch2 summary in the log'
        }
        if ([int]$Status[$key] -ne 0) { $passed = $false; $desc = "FAILED ($($Status[$key])): $desc" }
        $tests.Add("seed ${s}: $desc")
    }
    if ($passed -and $tests.Count -eq 0) { $passed = $false; $tests.Add('tests did not run') }
    return [pscustomobject]@{
        Leg = $Run.Leg; Config = $Run.Config; Image = $Image; Build = $build
        Tests = ($tests -join '; '); Seconds = $Seconds; OutDir = $OutDir; Passed = $passed
    }
}

function Format-LocalSummary {
    param([Parameter(Mandatory)][object[]]$Results)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('')
    $lines.Add('local CI summary')
    foreach ($r in $Results) {
        $verdict = 'FAIL'; if ($r.Passed) { $verdict = 'PASS' }
        $lines.Add(('{0}  {1,-15} {2,-7} {3,5:N1} min  build {4}' -f $verdict, $r.Leg, $r.Config, ($r.Seconds / 60), $r.Build))
        if ($r.Tests) { $lines.Add("      tests: $($r.Tests)") }
        $lines.Add("      image: $($r.Image); logs: $($r.OutDir)")
    }
    foreach ($r in $Results) {
        if ($r.Passed -or $r.Build -notlike 'FAILED*') { continue }
        $logs = @(Get-ChildItem -LiteralPath $r.OutDir -Filter 'build-*.log' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
        $errors = Get-BuildErrorLines -Path $logs -Max 30
        $lines.Add('')
        $lines.Add("first build errors, $($r.Leg) $($r.Config):")
        if ($errors.Count -eq 0) { $lines.Add('  (no compiler/linker error lines; read the build-*.log files)') }
        foreach ($e in $errors) { $lines.Add("  $e") }
    }
    $failed = @($Results | Where-Object { -not $_.Passed }).Count
    $lines.Add('')
    $lines.Add(('{0} of {1} leg(s) passed' -f ($Results.Count - $failed), $Results.Count))
    return ,$lines.ToArray()
}

# Runs a native command, echoing its combined stdout+stderr to the host and
# to $LogPath (UTF-8, no BOM -- Tee-Object on 5.1 writes UTF-16). Returns the
# exit code. EAP is relaxed inside: 5.1 wraps native stderr in ErrorRecords.
function Invoke-NativeLogged {
    param([Parameter(Mandatory)][string]$FilePath, [string[]]$ArgumentList = @(), [Parameter(Mandatory)][string]$LogPath)
    $full = Get-FullPath $LogPath
    $dir = Split-Path -Parent $full
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $writer = New-Object System.IO.StreamWriter($full, $false, (New-Object System.Text.UTF8Encoding($false)))
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @ArgumentList 2>&1 | ForEach-Object {
            $line = "$_"
            $writer.WriteLine($line)
            Write-Host $line
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
        $writer.Dispose()
    }
    return $code
}

# Runs a native command for its exit code and stdout lines; stderr dropped.
function Invoke-NativeQuiet {
    param([Parameter(Mandatory)][string]$FilePath, [string[]]$ArgumentList = @())
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = @(& $FilePath @ArgumentList 2>$null)
        return [pscustomobject]@{ Code = $LASTEXITCODE; Output = $out }
    } finally {
        $ErrorActionPreference = $prev
    }
}

# MSBuild.exe: from PATH (CI's microsoft/setup-msbuild), else vswhere -- the
# same query as ci/msbuild.cmd.
function Resolve-MSBuild {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere not found at $vswhere -- is Visual Studio installed?" }
    $found = @(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe') | Select-Object -First 1
    if (-not $found) { throw 'MSBuild not found via vswhere' }
    return $found
}

Export-ModuleMember -Function Resolve-LocalRuns, Get-SeedList, Get-LinuxJobCount, ConvertTo-Bytes,
    Set-WslConfigLimits, Test-WslConfigLimits, Get-Catch2Totals, Get-JUnitTotals, Compare-JUnitDirs,
    Get-BuildErrorLines, Read-LegStatus, Write-LegStatus, New-LegResult, Format-LocalSummary,
    Invoke-NativeLogged, Invoke-NativeQuiet, Resolve-MSBuild
```

Create `scripts/ci/compare-test-counts.ps1`:

```powershell
# compare-test-counts.ps1 -Baseline <dir> -Candidate <dir> -- compares two
# downloaded sets of CI test-results artifacts (`gh run download <run> -p
# 'test-results-*' -D <dir>`), keyed <leg>-<Config>[-gpu]: the tests and
# skipped counts must agree and the candidate must have no failures. The
# container-parity check of docs/superpowers/specs/2026-10-12-local-ci-pipeline-design.md
# s3.2; sub-project B reuses it for Jenkins side by side. Exit 0 = every key matches.
# -Key <pattern[]> limits the comparison (a local run has no -gpu lane).
param([Parameter(Mandatory)][string]$Baseline, [Parameter(Mandatory)][string]$Candidate, [string[]]$Key = @())
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$rows = Compare-JUnitDirs -Baseline $Baseline -Candidate $Candidate -Key $Key
$rows | Format-Table Key, BaselineTests, CandidateTests, BaselineSkipped, CandidateSkipped, CandidateFailures, Match -AutoSize | Out-Host
$bad = @($rows | Where-Object { -not $_.Match }).Count
if ($bad -gt 0) { Write-Host "compare-test-counts: $bad key(s) differ" -ForegroundColor Red; exit 1 }
Write-Host "compare-test-counts: all $($rows.Count) key(s) match"
exit 0
```

- [ ] **Step 4: Run the tests and see them pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci/tests/test-arcane-ci.ps1`
Expected: every line `ok ...`, last line `all checks passed`, exit 0. A `FAIL` line names its check; fix the module, never the expectation, unless the expectation contradicts this plan's interface block.

- [ ] **Step 5: Check line endings and commit**

```bash
git add scripts/ci/ArcaneCi.psm1 scripts/ci/compare-test-counts.ps1 scripts/ci/tests/test-arcane-ci.ps1
git ls-files --eol scripts/ci/ | cat     # *.ps1 show i/crlf or attr/text eol=crlf; *.psm1 see note
```

`.gitattributes` has `*.ps1 text eol=crlf` but no `*.psm1` rule. Add `*.psm1  text eol=crlf` under the `*.ps1` line, in the "Windows-only scripts" block, then `git add .gitattributes`. That rule is folded into this commit.

```bash
git commit -F - <<'EOF'
ci: ArcaneCi.psm1 -- the local-CI PowerShell helpers (leg/seed/job selection, .wslconfig limits, Catch2/JUnit totals, build-error grep, leg status + summary, logged native runs) with plain-PowerShell tests

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 2: `setup-desk.ps1`, and apply the WSL limits on the desk

**Files:**
- Create: `scripts/ci/setup-desk.ps1`

**Interfaces:**
- Consumes: `Set-WslConfigLimits`, `Invoke-NativeQuiet`, `Resolve-MSBuild` (Task 1).
- Produces: `%USERPROFILE%\.wslconfig` with `[wsl2] memory=24GB processors=16`. Tasks 3 and 8 need the memory for the image build and the Linux legs.

- [ ] **Step 1: Write the script**

```powershell
# setup-desk.ps1 -- one-time desk setup for scripts\ci\local.ps1 (docs/ci.md,
# "Local runs"). Idempotent; safe to re-run.
#   * %USERPROFILE%\.wslconfig: [wsl2] memory=24GB, processors=16 (every other
#     key kept; a changed file is backed up first)
#   * checks Docker Desktop (installed, running, Linux containers) and Visual
#     Studio's MSBuild (vswhere)
# It never runs `wsl --shutdown` itself: a changed .wslconfig applies after
# you run it, and that stops every WSL distro and Docker Desktop.
# Exit 0 when the desk is ready (a pending wsl --shutdown still counts).
param([string]$Memory = '24GB', [int]$Processors = 16)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$ok = $true

$path = Join-Path $env:USERPROFILE '.wslconfig'
$old = ''
if (Test-Path -LiteralPath $path) { $old = [System.IO.File]::ReadAllText($path) }
$new = Set-WslConfigLimits -Text $old -Memory $Memory -Processors $Processors
if ($new -ceq $old) {
    Write-Host "setup-desk: $path already has memory=$Memory, processors=$Processors"
} else {
    if ($old) {
        $backup = "$path.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item -LiteralPath $path -Destination $backup
        Write-Host "setup-desk: backed up $path -> $backup"
    }
    [System.IO.File]::WriteAllText($path, $new, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "setup-desk: wrote $path (memory=$Memory, processors=$Processors)"
    Write-Host 'setup-desk: run `wsl --shutdown` to apply it -- it stops every running WSL distro and Docker Desktop' -ForegroundColor Yellow
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host 'setup-desk: docker not found -- install Docker Desktop (WSL2 backend)' -ForegroundColor Red
    $ok = $false
} else {
    $info = Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('info', '--format', '{{.OSType}} {{.NCPU}} {{.MemTotal}}')
    if ($info.Code -ne 0 -or -not $info.Output) {
        Write-Host 'setup-desk: Docker Desktop is not running -- start it' -ForegroundColor Red
        $ok = $false
    } elseif (("$($info.Output[0])").Trim() -notlike 'linux *') {
        Write-Host "setup-desk: Docker reports '$($info.Output[0])' -- switch Docker Desktop to Linux containers" -ForegroundColor Red
        $ok = $false
    } else {
        Write-Host "setup-desk: docker ok (OS, CPUs, memory bytes: $($info.Output[0]))"
    }
}

try { Write-Host "setup-desk: MSBuild $(Resolve-MSBuild)" }
catch { Write-Host "setup-desk: $($_.Exception.Message) -- install Visual Studio 2026 with Desktop development with C++" -ForegroundColor Red; $ok = $false }

if (-not $ok) { exit 1 }
exit 0
```

- [ ] **Step 2: Run it on the desk (user-approved change, 2026-10-12)**

PowerShell: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\setup-desk.ps1`

Expected:
- `backed up ...\.wslconfig -> ...\.wslconfig.bak-<stamp>`, then `wrote ... (memory=24GB, processors=16)`, then the yellow `wsl --shutdown` line.
- Docker is stopped today, so the Docker check prints red and the exit code is 1.

`Get-Content $env:USERPROFILE\.wslconfig` shows `[wsl2]`, `memory=24GB`, `processors=16`.

- [ ] **Step 3: Apply it**

First check that nothing is building in WSL: `wsl -l --running` should list no busy distro, and the parked modularization run is finished. Then run `wsl --shutdown`, start Docker Desktop, and wait for it to report running.

Re-run Step 2's command. Expected:
- `already has memory=24GB, processors=16`;
- `docker ok (... linux 16 <~25e9> ...)`, i.e. NCPU 16 and MemTotal near 24 GiB;
- an MSBuild path;
- exit 0.

- [ ] **Step 4: Re-run is a no-op**

Run Step 2's command again. No new `.bak-` file appears (`dir $env:USERPROFILE\.wslconfig.bak-*` shows one backup), and the exit code is 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/ci/setup-desk.ps1
git commit -F - <<'EOF'
ci: scripts/ci/setup-desk.ps1 -- one-time desk setup for local CI runs (.wslconfig 24 GB / 16 processors, Docker + MSBuild checks), idempotent

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 3: The Linux CI image — `ci/linux/Dockerfile`, its input list, `verify-image.sh`

**Files:**
- Create: `ci/linux/Dockerfile`
- Create: `ci/linux/image-inputs.txt`
- Create: `ci/linux/verify-image.sh` (mode 100755)
- Modify: `.gitattributes` (Dockerfile LF rule)

**Interfaces:**
- Consumes: `scripts/build-sdl3.sh`, `scripts/build-vvl-linux.sh`, `scripts/fetch-dxc-linux.sh`, `scripts/fetch-premake.sh`, `scripts/sha256-check.sh` (unchanged).
- Produces:
  - Image target `build`.
  - `ENV ARCANE_TOOLS_DIR=/opt/arcane-tools`, holding `premake5/premake5` and `dxc-linux/{bin/dxc,lib/libdxcompiler.so,include/dxc}`.
  - `ENV SDL3_ROOT=/usr/local`.
  - ccache links in `/usr/lib/ccache`.
  - User `arcane` (1000:1000); `/work` and `/ccache` owned by 1000.
  - The build command (used verbatim by Tasks 7 and 8): `docker build --target build --build-context scripts=<repo>/scripts -f <repo>/ci/linux/Dockerfile -t <ref> <repo>/ci/linux`.

- [ ] **Step 1: Write the verification script first**

Create `ci/linux/verify-image.sh`:

```bash
#!/usr/bin/env bash
# verify-image.sh -- checks a built arcane-ci image holds everything the
# hosted Linux legs (.github/workflows/ci.yml) and scripts/ci/local.ps1 rely
# on. Runs INSIDE the image as its default user (root):
#   docker run --rm -v "$PWD/ci/linux:/v:ro" <image> bash /v/verify-image.sh
# scripts/ci/ci-image.sh runs it after every image build. Not an image input:
# it is never copied into the image, so editing it does not change the tag.
set -uo pipefail
fails=0
check() {
    local name=$1; shift
    if "$@" > /tmp/verify.out 2>&1; then
        echo "ok   $name"
    else
        echo "FAIL $name: $(head -c 400 /tmp/verify.out)"
        fails=$((fails + 1))
    fi
}
has() { command -v "$1" > /dev/null; }

check "default user is root" test "$(id -u)" = 0
check "g++-14 is GCC 14" sh -c 'g++-14 -dumpfullversion | grep -q "^14\."'
check "clang++-19 is Clang 19" sh -c 'clang++-19 --version | grep -q "version 19\."'
check "premake 5.0.0-beta8" sh -c '"${ARCANE_TOOLS_DIR:-}/premake5/premake5" --version | grep -q "5.0.0-beta8"'
check "dxc runs" "${ARCANE_TOOLS_DIR:-}/dxc-linux/bin/dxc" --version
check "libdxcompiler.so present" test -f "${ARCANE_TOOLS_DIR:-}/dxc-linux/lib/libdxcompiler.so"
check "dxc headers present" test -d "${ARCANE_TOOLS_DIR:-}/dxc-linux/include/dxc"
check "SDL3 3.2.22 via pkg-config" sh -c 'test "$(pkg-config --modversion sdl3)" = 3.2.22'
check "SDL3_ROOT=/usr/local" test "${SDL3_ROOT:-}" = /usr/local
check "validation layer 1.4.328" grep -q '"api_version": "1.4.328' /usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
check "no distro validation layer" test ! -e /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
check "lavapipe ICD" test -f /usr/share/vulkan/icd.d/lvp_icd.json
check "clang-19 sanitizer runtimes" sh -c 'find /usr/lib/llvm-19/lib/clang -name "libclang_rt.asan*" | grep -q . && find /usr/lib/llvm-19/lib/clang -name "libclang_rt.tsan*" | grep -q .'
for t in make cmake ninja pkg-config xvfb-run xauth weston weston-screenshooter xdotool import xwininfo \
         python3 ccache rsync git curl vulkaninfo; do
    check "tool $t" has "$t"
done
for c in gcc-14 g++-14 clang-19 clang++-19; do
    check "ccache link $c" sh -c "test \"\$(readlink -f /usr/lib/ccache/$c)\" = \"\$(readlink -f /usr/bin/ccache)\""
done
check "user arcane is uid 1000" test "$(id -u arcane)" = 1000
check "no ubuntu user" sh -c '! id ubuntu'
check "/work owned by 1000" test "$(stat -c %u /work)" = 1000
check "/ccache owned by 1000" test "$(stat -c %u /ccache)" = 1000
check "root runtime dir" test -d /run/user/0
check "no Arcane source baked in" test ! -e /work/premake5.lua
check "apt pinned to the snapshot" grep -q 'APT::Snapshot' /etc/apt/apt.conf.d/50arcane-snapshot
# The GPU lane's riskiest assumptions, as root (R13): lavapipe under Xvfb,
# and a headless weston (the windowed smoke's Wayland compositor).
check "Xvfb + lavapipe Vulkan as root" sh -c 'VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a vulkaninfo --summary | grep -qi llvmpipe'
check "weston headless starts as root" sh -c '
    d=$(mktemp -d); chmod 700 "$d"
    XDG_RUNTIME_DIR=$d weston --backend=headless --renderer=pixman --socket=probe --idle-time=0 > /tmp/weston.log 2>&1 &
    p=$!
    for _ in $(seq 50); do [ -S "$d/probe" ] && break; sleep 0.1; done
    rc=1; [ -S "$d/probe" ] && rc=0
    kill "$p" 2> /dev/null; cat /tmp/weston.log; exit $rc'

echo
if [ "$fails" -eq 0 ]; then echo "verify-image: all checks passed"; else echo "verify-image: $fails check(s) failed"; fi
[ "$fails" -eq 0 ]
```

- [ ] **Step 2: Watch it fail against a stock image**

```bash
docker run --rm -v "$(pwd -W)/ci/linux:/v:ro" ubuntu:24.04 bash /v/verify-image.sh | tail -n 5
```

Expected: many `FAIL` lines (no g++-14, no premake), then `verify-image: N check(s) failed`, and a non-zero exit (`echo ${PIPESTATUS[0]}` prints 1).

- [ ] **Step 3: Write the input list**

Create `ci/linux/image-inputs.txt`:

```
# The Linux CI image's tag inputs, in hash order (scripts/ci/image-tag.sh and
# image-tag.ps1 read this file). Every repo file ci/linux/Dockerfile copies
# must be listed, and every scripts/ entry must be copied by it -- both
# scripts refuse a mismatch. Paths are repo-relative, forward slashes.
ci/linux/Dockerfile
ci/linux/image-inputs.txt
scripts/build-sdl3.sh
scripts/build-vvl-linux.sh
scripts/fetch-dxc-linux.sh
scripts/fetch-premake.sh
scripts/sha256-check.sh
```

- [ ] **Step 4: Write the Dockerfile**

Create `ci/linux/Dockerfile`:

```dockerfile
# Arcane's Linux CI build image (docs/ci.md, "Image"): Ubuntu 24.04 with the
# exact toolchain the hosted Linux legs and scripts/ci/local.ps1 build with.
# Tools only -- the Arcane source is never baked in.
#
# Tag: scripts/ci/image-tag.sh hashes this file and every repo file it copies
# (ci/linux/image-inputs.txt; the script refuses a COPY outside that list).
# Repo files come in ONLY through the `scripts` build context, one COPY line:
#   docker build --target build --build-context scripts=scripts \
#       -f ci/linux/Dockerfile ci/linux
# Reproducible inputs: the base is pinned by digest, apt resolves from one
# Ubuntu archive snapshot (APT_SNAPSHOT), and SDL3 / premake / DXC / the
# Vulkan validation layer come from the repo's SHA-256- and commit-pinned
# scripts.
#
# The default user stays root: GitHub's container jobs mount the workspace
# owned by the runner's uid 1001 and expect root. scripts/ci/local.ps1 runs
# containers as arcane (uid 1000), which owns /work and /ccache, so a fresh
# named volume mounted there starts writable.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS build

LABEL org.opencontainers.image.source="https://github.com/StarworksDev/Arcane" \
      org.opencontainers.image.description="Arcane Linux CI build image: Ubuntu 24.04, GCC 14, Clang 19, tools only"

ARG APT_SNAPSHOT=20261012T000000Z
ENV DEBIAN_FRONTEND=noninteractive

# snapshot.ubuntu.com is HTTPS-only and the base image has no TLS roots, so
# ca-certificates comes first, from the archive the base image names -- the
# ONE package outside the snapshot (it supplies TLS roots only). Every later
# apt call resolves from the snapshot (APT::Snapshot).
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && echo "APT::Snapshot \"${APT_SNAPSHOT}\";" > /etc/apt/apt.conf.d/50arcane-snapshot \
 && rm -rf /var/lib/apt/lists/*

# CI's apt set (formerly ci.yml's "Toolchain (Linux)" step): the compilers,
# SDL3's X11/Wayland build deps, the Vulkan loader + Mesa lavapipe, Xvfb; the
# GPU-lane extras (weston, xdotool, imagemagick, x11-utils, python3); ccache
# and rsync for local runs; git and curl for the pinned fetch/build scripts;
# vulkan-tools for verify-image.sh; libclang-rt-19-dev (the sanitizer runtimes)
# for the ASan/UBSan lane that follows A (audit P1). The grep guard fails the build if apt did
# NOT read the snapshot.
RUN apt-get update \
 && ls /var/lib/apt/lists | grep -q '^snapshot\.ubuntu\.com_' \
 && apt-get install -y --no-install-recommends \
      g++-14 clang-19 make cmake ninja-build pkg-config \
      libclang-rt-19-dev libvulkan-dev mesa-vulkan-drivers vulkan-tools \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
      libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdecor-0-dev \
      libdrm-dev libgbm-dev libegl-dev libgl-dev \
      libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
      xvfb xauth \
      weston xdotool imagemagick x11-utils python3 \
      ccache rsync git curl \
 && rm -rf /var/lib/apt/lists/*

COPY --from=scripts --chmod=0755 build-sdl3.sh build-vvl-linux.sh fetch-dxc-linux.sh fetch-premake.sh sha256-check.sh /tmp/ci/scripts/

# SDL3 (Ubuntu 24.04 does not package it) into /usr/local, where pkg-config
# and the loader find it.
RUN /tmp/ci/scripts/build-sdl3.sh /usr/local && ldconfig

# The [gpu] lane's validation layer (build-vvl-linux.sh says why the distro's
# 1.3.275 cannot be used); its tag must resolve to the pinned commit.
RUN MAKE_JOBS="$(nproc)" VVL_WORK=/tmp/vvl /tmp/ci/scripts/build-vvl-linux.sh /usr/local \
 && ldconfig \
 && rm -rf /tmp/vvl

# premake + DXC land under /opt/arcane-tools; scripts/ci/build.sh links them
# into the checkout's ThirdParty/ locations each run.
RUN /tmp/ci/scripts/fetch-premake.sh \
 && /tmp/ci/scripts/fetch-dxc-linux.sh \
 && mkdir -p /opt/arcane-tools/premake5 \
 && mv /tmp/ci/ThirdParty/premake5/premake5 /opt/arcane-tools/premake5/premake5 \
 && mv /tmp/ci/ThirdParty/tools/dxc-linux /opt/arcane-tools/dxc-linux \
 && rm -rf /tmp/ci

# ccache front-ends for the versioned compilers. Only local runs put
# /usr/lib/ccache on PATH; hosted CI compiles without ccache.
RUN for c in gcc-14 g++-14 clang-19 clang++-19; do ln -sf ../../bin/ccache "/usr/lib/ccache/$c"; done

# arcane (uid 1000) for local runs: the base image's own uid-1000 user goes
# first. /run/user/0 is the root runtime dir the GPU lane may use.
RUN userdel --remove ubuntu \
 && groupadd --gid 1000 arcane \
 && useradd --uid 1000 --gid 1000 --create-home --shell /bin/bash arcane \
 && mkdir -p /work /ccache /run/user/0 /run/user/1000 \
 && chown 1000:1000 /work /ccache /run/user/1000 \
 && chmod 0700 /run/user/0 /run/user/1000

ENV ARCANE_TOOLS_DIR=/opt/arcane-tools \
    SDL3_ROOT=/usr/local

CMD ["bash"]
```

- [ ] **Step 5: Pin the Dockerfile to LF**

In `.gitattributes`, in the "Cross-platform scripts and source" block directly under `*.sh    text eol=lf`, add:

```
Dockerfile text eol=lf
```

Then confirm:

```bash
git add .gitattributes ci/linux/Dockerfile ci/linux/image-inputs.txt
git add --chmod=+x ci/linux/verify-image.sh
git ls-files --eol ci/linux/ | cat
```

Expected: `ci/linux/Dockerfile` shows `attr/text eol=lf`; `verify-image.sh` shows `eol=lf`.

- [ ] **Step 6: Build the image on the desk**

```bash
docker build --target build --build-context scripts=scripts -f ci/linux/Dockerfile -t arcane-ci:dev ci/linux 2>&1 | tee logs/image-dev.log
```

Expected: success, roughly 25–40 min (the validation layer dominates).

If the snapshot guard (`grep -q '^snapshot\.ubuntu\.com_'`) fails:
1. Run `docker run --rm ubuntu:24.04@sha256:534b... sh -c 'apt-get update >/dev/null; ls /var/lib/apt/lists'` with the snapshot config added, to see the real list file names.
2. Fix the pattern.
3. Never delete the guard.

If `userdel --remove ubuntu` fails on a missing mail spool, use `userdel ubuntu && rm -rf /home/ubuntu` instead.

- [ ] **Step 7: Verify the image**

```bash
docker run --rm -v "$(pwd -W)/ci/linux:/v:ro" arcane-ci:dev bash /v/verify-image.sh
docker inspect --format '{{.Config.User}}' arcane-ci:dev    # expected: empty (root)
```

Expected: every line `ok`, then `verify-image: all checks passed`, exit 0.

If `weston headless starts as root` FAILS, **stop and report to the user** with the printed weston log. The GPU lane's container plan depends on it (spec s3.2), and the fix (e.g. running the smoke step as a non-root user) changes `ci.yml`'s Linux job design. Do not work around it silently.

- [ ] **Step 8: Commit**

```bash
git commit -F - <<'EOF'
ci: ci/linux/Dockerfile -- the Linux CI build image (Ubuntu 24.04 by digest, apt snapshot 20261012T000000Z, GCC 14 + Clang 19, SDL3 / VVL / premake / DXC from the pinned scripts, root default + arcane uid 1000) and verify-image.sh

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 4: The image tag — `image-tag.sh`, its PowerShell twin, and their tests

**Files:**
- Create: `scripts/ci/image-tag.sh` (100755)
- Create: `scripts/ci/image-tag.ps1`
- Test: `scripts/ci/tests/test-image-tag.sh` (100755)

**Interfaces:**
- Consumes: `ci/linux/Dockerfile`, `ci/linux/image-inputs.txt` (Task 3).
- Produces:
  - `scripts/ci/image-tag.sh [--root DIR]` → prints `build-<16 hex>` on stdout and exits 0. It exits 1 with `image-tag: <reason>` on stderr for a refused input set.
  - `scripts/ci/image-tag.ps1 [-Root DIR]` → identical stdout and the same exit codes.
- Hash definition (both twins): SHA-256 over, for each input in list order, the bytes `<path>` NUL `<file bytes with every 0x0D removed>` NUL. The tag is `build-` + the first 16 lowercase hex digits.

- [ ] **Step 1: Write the failing test**

Create `scripts/ci/tests/test-image-tag.sh`:

```bash
#!/usr/bin/env bash
# Tests for scripts/ci/image-tag.sh, and its twin image-tag.ps1 when pwsh (or
# Windows PowerShell, on the desk) is on PATH -- spec s3.1. Runs in the
# image-tag job of .github/workflows/ci.yml and on the desk (Git Bash).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
TAG="$REPO/scripts/ci/image-tag.sh"
PS=""
if command -v pwsh > /dev/null 2>&1; then PS=pwsh
elif command -v powershell.exe > /dev/null 2>&1; then PS=powershell.exe; fi
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }

inputs() { tr -d '\r' < "$REPO/ci/linux/image-inputs.txt" | sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d'; }
# fixture: a temp root holding copies of the real inputs.
fixture() {
    local d f
    d=$(mktemp -d)
    for f in $(inputs); do mkdir -p "$d/$(dirname "$f")"; cp "$REPO/$f" "$d/$f"; done
    echo "$d"
}
tag_of() { "$TAG" --root "$1" 2> /dev/null; }
ps_tag_of() {
    local root=$1
    if [ "$PS" = powershell.exe ]; then root=$(cygpath -w "$root"); fi
    "$PS" -NoProfile -ExecutionPolicy Bypass -File "$REPO/scripts/ci/image-tag.ps1" -Root "$root" 2> /dev/null | tr -d '\r'
}

real=$("$TAG")
case "$real" in build-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ok "format: $real" ;; *) bad "format: '$real'" ;; esac
[ "$("$TAG")" = "$real" ] && ok "stable across runs" || bad "stable across runs"

base=$(fixture)
[ "$(tag_of "$base")" = "$real" ] && ok "fixture copy = repo" || bad "fixture copy = repo"

for f in $(inputs); do
    d=$(fixture); echo "# edit" >> "$d/$f"
    t=$(tag_of "$d" || true)
    [ -n "$t" ] && [ "$t" != "$real" ] && ok "edit changes the tag: $f" || bad "edit changes the tag: $f ($t)"
done

d=$(fixture)
for f in $(inputs); do sed 's/$/\r/' "$d/$f" > "$d/$f.crlf" && mv "$d/$f.crlf" "$d/$f"; done
[ "$(tag_of "$d")" = "$real" ] && ok "CRLF-only change keeps the tag" || bad "CRLF-only change keeps the tag"
crlf=$d

d=$(fixture); echo 'COPY --from=scripts extra.sh /tmp/ci/scripts/' >> "$d/ci/linux/Dockerfile"
if out=$("$TAG" --root "$d" 2>&1); then bad "unlisted COPY refused"; else
    case "$out" in *extra.sh*) ok "unlisted COPY refused" ;; *) bad "unlisted COPY refused, message: $out" ;; esac; fi

d=$(fixture); echo 'scripts/run-tests.sh' >> "$d/ci/linux/image-inputs.txt"; mkdir -p "$d/scripts"; cp "$REPO/scripts/run-tests.sh" "$d/scripts/"
if out=$("$TAG" --root "$d" 2>&1); then bad "listed but not copied refused"; else
    case "$out" in *run-tests.sh*) ok "listed but not copied refused" ;; *) bad "listed but not copied, message: $out" ;; esac; fi

d=$(fixture); echo 'ADD https://example.com/x /x' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "ADD refused" || ok "ADD refused"

d=$(fixture); echo 'COPY somefile /x' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "COPY from the context refused" || ok "COPY from the context refused"

d=$(fixture); printf 'COPY --from=scripts build-sdl3.sh \\\n    /tmp/x/\n' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "continued COPY refused" || ok "continued COPY refused"

d=$(fixture); printf 'FROM build AS runtime\nCOPY --from=build /usr/local/lib/libSDL3.so.0 /usr/lib/\n' >> "$d/ci/linux/Dockerfile"
"$TAG" --root "$d" > /dev/null 2>&1 && ok "COPY from a declared stage allowed" || bad "COPY from a declared stage allowed"

d=$(fixture); rm "$d/scripts/sha256-check.sh"
"$TAG" --root "$d" > /dev/null 2>&1 && bad "missing input refused" || ok "missing input refused"

if git -C "$REPO" rev-parse --git-dir > /dev/null 2>&1; then
    case "$(git -C "$REPO" check-attr eol -- ci/linux/Dockerfile)" in *": eol: lf") ok ".gitattributes: Dockerfile eol=lf" ;; *) bad ".gitattributes: Dockerfile eol=lf" ;; esac
    case "$(git -C "$REPO" check-attr eol -- scripts/ci/image-tag.sh)" in *": eol: lf") ok ".gitattributes: *.sh eol=lf" ;; *) bad ".gitattributes: *.sh eol=lf" ;; esac
else
    echo "skip .gitattributes checks (no git checkout)"
fi

if [ -n "$PS" ]; then
    [ "$(ps_tag_of "$REPO")" = "$real" ] && ok "image-tag.ps1 = image-tag.sh (repo)" || bad "image-tag.ps1 = image-tag.sh (repo): $(ps_tag_of "$REPO")"
    [ "$(ps_tag_of "$crlf")" = "$real" ] && ok "image-tag.ps1 = image-tag.sh (CRLF copy)" || bad "image-tag.ps1 = image-tag.sh (CRLF copy)"
    d=$(fixture); echo 'COPY --from=scripts extra.sh /tmp/ci/scripts/' >> "$d/ci/linux/Dockerfile"
    [ -z "$(ps_tag_of "$d")" ] && ok "image-tag.ps1 refuses an unlisted COPY" || bad "image-tag.ps1 refuses an unlisted COPY"
else
    echo "skip image-tag.ps1 checks (no pwsh / powershell.exe)"
fi

echo
[ "$fails" -eq 0 ] && echo "test-image-tag: all passed" || echo "test-image-tag: $fails failure(s)"
[ "$fails" -eq 0 ]
```

The last ps1 case checks for an empty stdout, because `ps_tag_of`'s pipeline exit status is `tr`'s. A refused input set prints nothing on stdout.

- [ ] **Step 2: Run it and watch it fail**

```bash
git add --chmod=+x scripts/ci/tests/test-image-tag.sh
scripts/ci/tests/test-image-tag.sh
```

Expected: it exits immediately with `image-tag.sh: No such file or directory` (set -e on the first `$("$TAG")`). Exit is non-zero.

- [ ] **Step 3: Write `image-tag.sh`**

```sh
#!/bin/sh
# image-tag.sh [--root DIR] -- prints the Linux CI image tag, build-<16 hex>:
# the first 16 hex digits of SHA-256 over every input listed in
# ci/linux/image-inputs.txt, in that order, each as <path> NUL <bytes with
# every CR removed> NUL (so a CRLF checkout hashes like an LF one). The one
# definition of "which image" -- ci.yml's image-tag job and the desk use it;
# scripts/ci/image-tag.ps1 is the byte-for-byte PowerShell twin. Image:
# ghcr.io/starworksdev/arcane-ci:<tag>.
#
# The list is checked, not trusted: every repo file the Dockerfile copies
# must be listed, and every listed scripts/ file must be copied. Repo files
# may only enter through ONE-LINE `COPY --from=scripts <file>... <dest>`;
# ADD, a continued COPY, and a COPY from the build context are refused
# (`COPY --from=<a stage declared with FROM ... AS>` is fine).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ "${1:-}" = "--root" ]; then ROOT=$(cd "${2:?--root needs a directory}" && pwd); fi
LIST="$ROOT/ci/linux/image-inputs.txt"
DOCKERFILE="$ROOT/ci/linux/Dockerfile"
fail() { echo "image-tag: $*" >&2; exit 1; }
[ -f "$LIST" ] || fail "missing $LIST"
[ -f "$DOCKERFILE" ] || fail "missing $DOCKERFILE"

inputs=$(tr -d '\r' < "$LIST" | sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d')

refs=$(tr -d '\r' < "$DOCKERFILE" | awk '
    /^[[:space:]]*#/ { next }
    toupper($1) == "FROM" { for (i = 2; i < NF; i++) if (toupper($i) == "AS") stage[$(i + 1)] = 1; next }
    toupper($1) == "ADD"  { print "!ADD " $0; next }
    toupper($1) == "COPY" {
        if ($NF == "\\") { print "!CONT " $0; next }
        from = ""; n = 0
        for (i = 2; i <= NF; i++) {
            if (substr($i, 1, 2) == "--") { if (substr($i, 1, 7) == "--from=") from = substr($i, 8); continue }
            arg[++n] = $i
        }
        if (from != "" && (from in stage)) next
        if (from != "scripts") { print "!COPY " $0; next }
        for (i = 1; i < n; i++) print "scripts/" arg[i]
    }')

bad=$(printf '%s\n' "$refs" | grep '^!' | head -n 1 || true)
case "$bad" in
    '') ;;
    '!ADD '*)  fail "ci/linux/Dockerfile uses ADD (use a one-line COPY --from=scripts): ${bad#!ADD }" ;;
    '!CONT '*) fail "a COPY in ci/linux/Dockerfile continues onto the next line (keep it on one line): ${bad#!CONT }" ;;
    *)         fail "every repo file must come in as COPY --from=scripts <file>... <dest>: ${bad#!COPY }" ;;
esac

in_list() { printf '%s\n' "$2" | grep -qxF "$1"; }
for r in $refs; do
    in_list "$r" "$inputs" || fail "ci/linux/Dockerfile copies $r, which is not in ci/linux/image-inputs.txt"
done
in_list ci/linux/Dockerfile "$inputs" || fail "ci/linux/image-inputs.txt must list ci/linux/Dockerfile"
in_list ci/linux/image-inputs.txt "$inputs" || fail "ci/linux/image-inputs.txt must list itself"
for f in $inputs; do
    case "$f" in
        scripts/*) in_list "$f" "$refs" || fail "$f is listed in ci/linux/image-inputs.txt but ci/linux/Dockerfile does not copy it" ;;
    esac
    [ -f "$ROOT/$f" ] || fail "missing input $f"
done

hash() { if command -v sha256sum > /dev/null 2>&1; then sha256sum; else shasum -a 256; fi; }
sum=$(for f in $inputs; do printf '%s\000' "$f"; tr -d '\r' < "$ROOT/$f"; printf '\000'; done | hash | cut -c1-16)
echo "build-$sum"
```

- [ ] **Step 4: Write `image-tag.ps1`**

```powershell
# image-tag.ps1 [-Root DIR] -- the PowerShell twin of scripts/ci/image-tag.sh:
# prints the Linux CI image tag (build-<16 hex>) from the same inputs, the
# same checks and the same bytes (<path> NUL <bytes minus every CR> NUL, in
# ci/linux/image-inputs.txt order). scripts/ci/tests/test-image-tag.sh asserts
# both print the same tag. scripts\ci\local.ps1 uses it, so the desk resolves
# the tag without starting a container. Exit 1 + "image-tag: <reason>" on
# stderr when the inputs are refused (read image-tag.sh's header for the rules).
param([string]$Root)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$Root = (Resolve-Path -LiteralPath $Root).ProviderPath

function Fail([string]$Message) { [Console]::Error.WriteLine("image-tag: $Message"); exit 1 }
function Read-Text([string]$Rel) {
    $p = Join-Path $Root $Rel
    if (-not (Test-Path -LiteralPath $p)) { Fail "missing $p" }
    return ([System.IO.File]::ReadAllText($p)).Replace("`r", '')
}

$inputs = @()
foreach ($l in ((Read-Text 'ci/linux/image-inputs.txt') -split "`n")) {
    $t = ($l -replace '#.*$', '').Trim()
    if ($t) { $inputs += $t }
}

$stages = @{}
$refs = @()
foreach ($line in ((Read-Text 'ci/linux/Dockerfile') -split "`n")) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $tok = @($t -split '\s+')
    $kw = $tok[0].ToUpperInvariant()
    if ($kw -eq 'FROM') {
        for ($i = 1; $i -lt $tok.Count - 1; $i++) { if ($tok[$i] -ieq 'AS') { $stages[$tok[$i + 1]] = $true } }
        continue
    }
    if ($kw -eq 'ADD') { Fail "ci/linux/Dockerfile uses ADD (use a one-line COPY --from=scripts): $t" }
    if ($kw -ne 'COPY') { continue }
    if ($tok[$tok.Count - 1] -eq '\') { Fail "a COPY in ci/linux/Dockerfile continues onto the next line (keep it on one line): $t" }
    $from = ''
    $rest = @()
    for ($i = 1; $i -lt $tok.Count; $i++) {
        if ($tok[$i].StartsWith('--')) { if ($tok[$i].StartsWith('--from=')) { $from = $tok[$i].Substring(7) } }
        else { $rest += $tok[$i] }
    }
    if ($from -and $stages.ContainsKey($from)) { continue }
    if ($from -ne 'scripts') { Fail "every repo file must come in as COPY --from=scripts <file>... <dest>: $t" }
    for ($i = 0; $i -lt $rest.Count - 1; $i++) { $refs += "scripts/$($rest[$i])" }
}

foreach ($r in $refs) { if ($inputs -cnotcontains $r) { Fail "ci/linux/Dockerfile copies $r, which is not in ci/linux/image-inputs.txt" } }
if ($inputs -cnotcontains 'ci/linux/Dockerfile') { Fail 'ci/linux/image-inputs.txt must list ci/linux/Dockerfile' }
if ($inputs -cnotcontains 'ci/linux/image-inputs.txt') { Fail 'ci/linux/image-inputs.txt must list itself' }
foreach ($f in $inputs) {
    if ($f.StartsWith('scripts/') -and ($refs -cnotcontains $f)) { Fail "$f is listed in ci/linux/image-inputs.txt but ci/linux/Dockerfile does not copy it" }
    if (-not (Test-Path -LiteralPath (Join-Path $Root $f))) { Fail "missing input $f" }
}

$ms = New-Object System.IO.MemoryStream
foreach ($f in $inputs) {
    $name = [System.Text.Encoding]::UTF8.GetBytes($f)
    $ms.Write($name, 0, $name.Length)
    $ms.WriteByte(0)
    foreach ($b in [System.IO.File]::ReadAllBytes((Join-Path $Root $f))) { if ($b -ne 13) { $ms.WriteByte($b) } }
    $ms.WriteByte(0)
}
$sha = [System.Security.Cryptography.SHA256]::Create()
$hex = -join ($sha.ComputeHash($ms.ToArray()) | ForEach-Object { $_.ToString('x2') })
Write-Output "build-$($hex.Substring(0, 16))"
exit 0
```

- [ ] **Step 5: Run the tests and see them pass**

```bash
git add --chmod=+x scripts/ci/image-tag.sh
git add scripts/ci/image-tag.ps1
scripts/ci/tests/test-image-tag.sh
```

Expected: every line `ok` (on the desk, the ps1 checks run through `powershell.exe`), then `test-image-tag: all passed`, exit 0. Also run it inside the image to prove the Linux tools agree:

```bash
docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash -c 'cp -r /r /tmp/r && cd /tmp/r && scripts/ci/tests/test-image-tag.sh' | tail -n 3
```

Expected: `test-image-tag: all passed`. The copy is needed because the bind mount is read-only and `fixture` writes only to mktemp, so this is just isolation. The `.gitattributes` checks print `skip` there, because `/tmp/r/.git` is the worktree's Windows-path file.

- [ ] **Step 6: Commit**

```bash
git add scripts/ci/tests/test-image-tag.sh
git commit -F - <<'EOF'
ci: scripts/ci/image-tag.{sh,ps1} -- the content-addressed Linux image tag (build-<16 hex> over ci/linux/image-inputs.txt, CR-insensitive) with a checked input list, and its tests (spec s3.1)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 5: `scripts/ci/build.sh` — the Linux/macOS build sequence

**Files:**
- Create: `scripts/ci/build.sh` (100755)
- Test: `scripts/ci/tests/test-build-sh.sh` (100755)

**Interfaces:**
- Consumes: `scripts/arcane-bin-dir.sh <Config>` (prints `bin/<Config>-<system>-<arch>-md`), and `ARCANE_TOOLS_DIR` (Task 3 image).
- Produces: `scripts/ci/build.sh <Debug|Release|Dist> [-j N]`, with the `MAKE_JOBS` env as the default for `-j` and then `nproc` / `sysctl -n hw.ncpu`.
  - Exit codes: 0, the failing step's code, or 2 for usage / missing premake / unknown `CC`.
  - Logs: `logs/build-{generate,engine,referencegame,tests}.log`; stale `logs/build-*.log` are removed first.

- [ ] **Step 1: Write the failing test**

Create `scripts/ci/tests/test-build-sh.sh`:

```bash
#!/usr/bin/env bash
# Tests for scripts/ci/build.sh against a fake checkout: stub premake5, make
# and arcbuild record their arguments, so the step order, flags, logs and the
# stop-at-first-failure rule are checked without a compiler. Runs in the
# image-tag job of .github/workflows/ci.yml and on any Linux/macOS shell.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }

# stub <path> <name>: records "<name> <args>" in $CALLS. premake exits
# $FAIL_PREMAKE when set; make exits $FAIL_CODE (default 3) when its
# arguments contain the word $FAIL_MAKE_ON.
stub() {
    cat > "$1" <<EOF
#!/bin/sh
echo "$2 \$*" >> "\$CALLS"
if [ "$2" = make ] && [ -n "\${FAIL_MAKE_ON:-}" ]; then
    case " \$* " in *" \$FAIL_MAKE_ON "*) exit "\${FAIL_CODE:-3}" ;; esac
fi
if [ "$2" = premake ] && [ -n "\${FAIL_PREMAKE:-}" ]; then exit "\$FAIL_PREMAKE"; fi
exit 0
EOF
    chmod +x "$1"
}

# fixture [--no-premake]: prints a fake checkout root.
fixture() {
    local d c bin
    d=$(mktemp -d)
    mkdir -p "$d/scripts/ci" "$d/ThirdParty/premake5" "$d/ReferenceProject/Binaries" "$d/fakebin"
    cp "$REPO/scripts/ci/build.sh" "$d/scripts/ci/build.sh"
    cp "$REPO/scripts/arcane-bin-dir.sh" "$d/scripts/arcane-bin-dir.sh"
    chmod +x "$d/scripts/ci/build.sh" "$d/scripts/arcane-bin-dir.sh"
    if [ "${1:-}" != --no-premake ]; then stub "$d/ThirdParty/premake5/premake5" premake; fi
    stub "$d/fakebin/make" make
    for c in Debug Release Dist; do
        bin="$d/$("$d/scripts/arcane-bin-dir.sh" "$c")/arcbuild"
        mkdir -p "$bin"
        stub "$bin/arcbuild" arcbuild
    done
    echo "$d"
}

# run <root> [VAR=value...] <command...>: sets RC; $root/calls.txt, $root/out.txt.
run() {
    local root=$1; shift
    : > "$root/calls.txt"
    set +e
    (cd "$root" && env -u CC -u CXX -u MAKE_JOBS -u ARCANE_TOOLS_DIR \
        PATH="$root/fakebin:$PATH" CALLS="$root/calls.txt" "$@") > "$root/out.txt" 2>&1
    RC=$?
    set -e
}
first_call() { head -n 1 "$1/calls.txt"; }

d=$(fixture); run "$d" CC=gcc-14 scripts/ci/build.sh Debug -j 4
expected="premake --cc=gcc gmake
make -k -j4 config=debug ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient
arcbuild build --project ReferenceProject --config Debug --sdk $d
make -k -j4 config=debug ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests"
[ "$RC" -eq 0 ] && [ "$(cat "$d/calls.txt")" = "$expected" ] && ok "steps, order and flags" \
    || bad "steps, order and flags (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"
all=1; for l in generate engine referencegame tests; do [ -f "$d/logs/build-$l.log" ] || all=0; done
[ "$all" -eq 1 ] && ok "four step logs" || bad "four step logs: $(ls "$d/logs" 2>&1)"

d=$(fixture); run "$d" CC=clang-19 scripts/ci/build.sh Release
[ "$(first_call "$d")" = "premake --cc=clang gmake" ] && ok "clang from CC" || bad "clang from CC: $(first_call "$d")"
grep -q "config=release" "$d/calls.txt" && ok "config lower-cased for make" || bad "config lower-cased for make"

d=$(fixture); run "$d" MAKE_JOBS=5 scripts/ci/build.sh Dist
grep -q -- "-j5 config=dist" "$d/calls.txt" && ok "MAKE_JOBS is the default -j" || bad "MAKE_JOBS is the default -j: $(cat "$d/calls.txt")"

d=$(fixture); run "$d" MAKE_JOBS=5 scripts/ci/build.sh Debug -j7
grep -q -- "-j7 config=debug" "$d/calls.txt" && ok "-jN beats MAKE_JOBS" || bad "-jN beats MAKE_JOBS"

if [ "$(uname -s)" = Linux ]; then
    d=$(fixture); run "$d" scripts/ci/build.sh Debug
    [ "$(first_call "$d")" = "premake --cc=gcc gmake" ] && ok "no CC on Linux -> gcc" || bad "no CC on Linux -> gcc"
fi

d=$(fixture); run "$d" FAIL_MAKE_ON=ArcaneCore FAIL_CODE=3 scripts/ci/build.sh Debug
[ "$RC" -eq 3 ] && ! grep -q '^arcbuild' "$d/calls.txt" && [ ! -f "$d/logs/build-tests.log" ] \
    && ok "engine failure stops the build with its code" || bad "engine failure stops the build (rc $RC): $(cat "$d/calls.txt")"

d=$(fixture); run "$d" FAIL_PREMAKE=4 scripts/ci/build.sh Debug
[ "$RC" -eq 4 ] && [ "$(wc -l < "$d/calls.txt" | tr -d ' ')" = 1 ] && ok "generate failure stops the build" || bad "generate failure stops the build (rc $RC)"

d=$(fixture); mkdir -p "$d/logs"; echo stale > "$d/logs/build-tests.log"
run "$d" FAIL_PREMAKE=4 scripts/ci/build.sh Debug
[ ! -f "$d/logs/build-tests.log" ] && ok "stale step logs removed" || bad "stale step logs removed"

d=$(fixture); run "$d" scripts/ci/build.sh debug
[ "$RC" -eq 2 ] && [ ! -s "$d/calls.txt" ] && ok "bad config -> 2" || bad "bad config -> 2 (rc $RC)"
d=$(fixture); run "$d" scripts/ci/build.sh Debug -j x
[ "$RC" -eq 2 ] && ok "bad -j -> 2" || bad "bad -j -> 2 (rc $RC)"
d=$(fixture); run "$d" CC=icc scripts/ci/build.sh Debug
[ "$RC" -eq 2 ] && ok "unknown compiler family -> 2" || bad "unknown compiler family -> 2 (rc $RC)"

tools=$(mktemp -d); mkdir -p "$tools/premake5" "$tools/dxc-linux/bin"; stub "$tools/premake5/premake5" premake
d=$(fixture --no-premake); run "$d" ARCANE_TOOLS_DIR="$tools" scripts/ci/build.sh Debug
[ "$RC" -eq 0 ] && [ -L "$d/ThirdParty/premake5/premake5" ] && [ "$(readlink "$d/ThirdParty/tools/dxc-linux")" = "$tools/dxc-linux" ] \
    && ok "ARCANE_TOOLS_DIR linked into ThirdParty" || bad "ARCANE_TOOLS_DIR linked (rc $RC): $(cat "$d/out.txt")"
run "$d" ARCANE_TOOLS_DIR="$tools" scripts/ci/build.sh Debug
[ "$RC" -eq 0 ] && ok "re-linking is idempotent" || bad "re-linking is idempotent (rc $RC)"

d=$(fixture); run "$d" ARCANE_TOOLS_DIR="$tools" scripts/ci/build.sh Debug
[ ! -L "$d/ThirdParty/premake5/premake5" ] && grep -q "keeping the existing" "$d/out.txt" \
    && ok "an existing real premake is kept" || bad "an existing real premake is kept"

d=$(fixture --no-premake); run "$d" scripts/ci/build.sh Debug
[ "$RC" -eq 2 ] && grep -q "fetch-premake.sh" "$d/out.txt" && ok "missing premake names the fix" || bad "missing premake names the fix (rc $RC)"

# Portability lints (comment lines ignored): bash 3.2 has no associative
# arrays, case-modifying expansions, mapfile/readarray, namerefs, |&, coproc.
LINTED="$REPO/scripts/ci/build.sh"
hits=$(grep -nE 'declare -A|typeset -A|mapfile|readarray|\$\{[A-Za-z_][A-Za-z0-9_]*(,,|\^\^)|local -n|\|&|coproc' $LINTED | grep -vE '^([^:]*:)?[0-9]+:[[:space:]]*#' || true)
[ -z "$hits" ] && ok "bash 3.2 lint" || bad "bash 3.2 lint: $hits"
gits=$(grep -nE '(^|[^A-Za-z0-9_./-])git([[:space:]]|$)' "$REPO/scripts/ci/build.sh" | grep -vE '^[0-9]+:[[:space:]]*#' || true)
[ -z "$gits" ] && ok "build.sh calls no git" || bad "build.sh calls git: $gits"

echo
[ "$fails" -eq 0 ] && echo "test-build-sh: all passed" || echo "test-build-sh: $fails failure(s)"
[ "$fails" -eq 0 ]
```

- [ ] **Step 2: Run it and watch it fail**

```bash
git add --chmod=+x scripts/ci/tests/test-build-sh.sh
docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash /r/scripts/ci/tests/test-build-sh.sh 2>&1 | tail -n 5
```

Expected: `cp: cannot stat '/r/scripts/ci/build.sh'` from the first fixture, and a non-zero exit. The suite runs in the image (bash 5, GNU tools) here; it also runs in Git Bash on the desk.

- [ ] **Step 3: Write `build.sh`**

```bash
#!/usr/bin/env bash
# build.sh <Debug|Release|Dist> [-j N] -- THE Linux/macOS build sequence:
# .github/workflows/ci.yml and scripts/ci/local.ps1 run exactly this. The
# Windows twin is scripts/ci/build.ps1 (same steps, same log names).
#   1. generate  premake5 --cc=<gcc|clang> gmake      (family from $CC)
#   2. engine    make -k  ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient
#   3. game      arcbuild build --project ReferenceProject --config <cfg> --sdk <checkout>
#   4. hosts     make -k  ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
#                (their postbuild stages ReferenceProject, Binaries/ included)
# Stops at the first failing step and exits with its code (make -k inside a
# step still reports every error of that step). Logs:
# logs/build-{generate,engine,referencegame,tests}.log; stale ones go first.
# Jobs: -j N, else $MAKE_JOBS, else the CPU count.
#
# Tools: with ARCANE_TOOLS_DIR set (the CI image), premake5 and the Linux DXC
# are symlinked from it into ThirdParty/ (an existing real file or directory
# there is kept). Otherwise they must already be in place
# (scripts/fetch-premake.sh; scripts/fetch-dxc-linux.sh or
# scripts/fetch-vulkan-sdk-macos.sh).
#
# Portability: bash 3.2 (macOS /bin/bash) + POSIX tools -- no associative
# arrays, case-modifying expansions or mapfile. No git: desk checkouts are
# worktrees whose .git file points at a Windows path that does not exist in
# the container; revision info, if ever needed, comes in through the env.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
usage() { echo "usage: build.sh <Debug|Release|Dist> [-j N]" >&2; exit 2; }

CONFIG="${1:-}"
[ $# -gt 0 ] && shift
case "$CONFIG" in Debug|Release|Dist) ;; *) usage ;; esac
JOBS="${MAKE_JOBS:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        -j) [ $# -ge 2 ] || usage; JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        *) usage ;;
    esac
done
if [ -z "$JOBS" ]; then JOBS=$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 2); fi
case "$JOBS" in ''|*[!0-9]*) echo "build: -j needs a number (got '$JOBS')" >&2; exit 2 ;; esac

case "${CC:-}" in
    *clang*) PREMAKE_CC=clang ;;
    *gcc*|*g++*) PREMAKE_CC=gcc ;;
    '') if [ "$(uname -s)" = Darwin ]; then PREMAKE_CC=clang; else PREMAKE_CC=gcc; fi ;;
    *) echo "build: cannot tell gcc from clang in CC='$CC'" >&2; exit 2 ;;
esac

cd "$ROOT" || exit 2

link_tool() {   # link_tool <checkout path> <tool path>
    local dest=$1 src=$2
    if [ -L "$dest" ] || [ ! -e "$dest" ]; then
        mkdir -p "$(dirname "$dest")"
        ln -sfn "$src" "$dest"
    else
        echo "build: keeping the existing $dest (not linking $src)"
    fi
}
if [ -n "${ARCANE_TOOLS_DIR:-}" ]; then
    link_tool ThirdParty/premake5/premake5 "$ARCANE_TOOLS_DIR/premake5/premake5"
    link_tool ThirdParty/tools/dxc-linux "$ARCANE_TOOLS_DIR/dxc-linux"
fi
if [ ! -x ThirdParty/premake5/premake5 ]; then
    echo "build: ThirdParty/premake5/premake5 is missing -- run scripts/fetch-premake.sh (the CI image sets ARCANE_TOOLS_DIR instead)" >&2
    exit 2
fi

mkdir -p logs
rm -f logs/build-*.log
config_lc=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
bindir=$(scripts/arcane-bin-dir.sh "$CONFIG") || exit 2

step() {   # step <name> <log> <command...>
    local name=$1 log=$2
    shift 2
    echo "build: [$name] $*"
    "$@" 2>&1 | tee "logs/$log"
    local rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        echo "build: [$name] FAILED (exit $rc) -- logs/$log" >&2
        exit "$rc"
    fi
}

step generate build-generate.log ThirdParty/premake5/premake5 --cc="$PREMAKE_CC" gmake
step engine build-engine.log make -k -j"$JOBS" config="$config_lc" \
    ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient
step game build-referencegame.log "$bindir/arcbuild/arcbuild" build --project ReferenceProject \
    --config "$CONFIG" --sdk "$ROOT"
ls -l ReferenceProject/Binaries/
step hosts build-tests.log make -k -j"$JOBS" config="$config_lc" \
    ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
echo "build: $CONFIG ok"
```

- [ ] **Step 4: Run the tests and see them pass**

```bash
git add --chmod=+x scripts/ci/build.sh
docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash /r/scripts/ci/tests/test-build-sh.sh 2>&1 | tail -n 4
scripts/ci/tests/test-build-sh.sh 2>&1 | tail -n 4     # Git Bash on the desk (the no-CC case is skipped: uname is MINGW)
```

Expected: `test-build-sh: all passed` from both runs, exit 0.

On Git Bash, `arcane-bin-dir.sh` refuses `MINGW*` ("unsupported host"), so every fixture fails. In that case, run the suite only in the image and record that here. The image run is the one CI executes.

- [ ] **Step 5: A real build in the image (smoke)**

Run gcc Debug against a throwaway copy of the checkout, as `arcane` (the local user):

```bash
docker volume create arcane-src-smoke
docker run --rm --user 1000:1000 -v "$(pwd -W):/src:ro" -v arcane-src-smoke:/work -e CC=gcc-14 -e CXX=g++-14 arcane-ci:dev \
  bash -c 'rsync -a --exclude=/.git --exclude=/bin/ --exclude=/logs/ /src/ /work/ && cd /work && scripts/ci/build.sh Debug -j 12'
```

Expected:
- `build: Debug ok`;
- `ReferenceProject/Binaries/` lists `libReferenceGame.so`;
- 20–40 min cold.

This proves the image builds the engine. Task 8 replaces this ad hoc rsync with the real filter. Then run `docker volume rm arcane-src-smoke`.

If a library or tool is missing:
1. Add the apt package to the Dockerfile's list with the consumer in the comment.
2. Rebuild `arcane-ci:dev`, and rerun `verify-image.sh` and this step.
3. Commit the Dockerfile change in this task's commit.

- [ ] **Step 6: Commit**

```bash
git add scripts/ci/tests/test-build-sh.sh
git commit -F - <<'EOF'
ci: scripts/ci/build.sh -- the Linux/macOS build sequence moves out of ci.yml (generate, engine, ReferenceGame via arcbuild, hosts+tests; stops at the first failing step; links the image's premake/DXC), bash 3.2-safe, no git; stub-driven tests

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 6: `scripts/ci/build.ps1` — the Windows build sequence

**Files:**
- Create: `scripts/ci/build.ps1`

**Interfaces:**
- Consumes: `Invoke-NativeLogged`, `Resolve-MSBuild` (Task 1); `GenerateProjects.bat` (`_APH_NOPAUSE=1`).
- Produces: `scripts/ci/build.ps1 <Debug|Release|Dist> [-Jobs N | -j N]`.
  - Exit codes: 0, or the failing step's code.
  - Logs: `logs\build-{generate,engine,referencegame,tests}.log` (UTF-8); stale ones are removed first.

- [ ] **Step 1: Write the script**

```powershell
# build.ps1 <Debug|Release|Dist> [-Jobs N] -- THE Windows build sequence:
# .github/workflows/ci.yml and scripts\ci\local.ps1 run exactly this. The
# Linux/macOS twin is scripts/ci/build.sh (same steps, same log names).
#   1. generate  GenerateProjects.bat (premake vs2026)
#   2. engine    msbuild Arcane.slnx  (the whole solution)
#   3. game      arcbuild build --project ReferenceProject --config <cfg> --sdk <checkout>
#   4. hosts     msbuild Arcane.slnx again, so every host's postbuild restages
#                the game module
# Stops at the first failing step and exits with its code. Logs:
# logs\build-{generate,engine,referencegame,tests}.log (UTF-8); stale ones go
# first. -Jobs N -> msbuild /m:N (default /m). MSBuild comes from PATH (CI's
# setup-msbuild) or vswhere (the desk).
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('Debug', 'Release', 'Dist')][string]$Config,
    [Alias('j')][int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Remove-Item -Path (Join-Path $logs 'build-*.log') -ErrorAction SilentlyContinue

$msbuild = Resolve-MSBuild
$m = '/m'
if ($Jobs -gt 0) { $m = "/m:$Jobs" }
$solution = Join-Path $root 'Arcane.slnx'
$env:_APH_NOPAUSE = '1'

function Step([string]$Name, [string]$Log, [string]$File, [string[]]$Arguments) {
    Write-Host "build: [$Name] $File $($Arguments -join ' ')"
    $code = Invoke-NativeLogged -FilePath $File -ArgumentList $Arguments -LogPath (Join-Path $logs $Log)
    if ($code -ne 0) {
        Write-Host "build: [$Name] FAILED (exit $code) -- logs\$Log" -ForegroundColor Red
        exit $code
    }
}

Step 'generate' 'build-generate.log' 'cmd.exe' @('/c', (Join-Path $root 'GenerateProjects.bat'))
Step 'engine' 'build-engine.log' $msbuild @($solution, "/p:Configuration=$Config", $m, '/nologo', '/v:minimal')
Step 'game' 'build-referencegame.log' (Join-Path $root "bin\$Config-windows-x86_64-md\arcbuild\arcbuild.exe") `
    @('build', '--project', (Join-Path $root 'ReferenceProject'), '--config', $Config, '--sdk', $root)
Step 'hosts' 'build-tests.log' $msbuild @($solution, "/p:Configuration=$Config", $m, '/nologo', '/v:minimal')
Write-Host "build: $Config ok"
exit 0
```

- [ ] **Step 2: Prove it stops at the first failure**

PowerShell, in the worktree:

```powershell
$env:VCPKG_ROOT_SAVED = $env:VCPKG_ROOT; $env:VCPKG_ROOT = 'D:\no-such-vcpkg'
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\build.ps1 Debug; "exit=$LASTEXITCODE"
Test-Path logs\build-generate.log, logs\build-engine.log
$env:VCPKG_ROOT = $env:VCPKG_ROOT_SAVED
```

Expected:
- `build: [generate] FAILED (exit 1) -- logs\build-generate.log`, then `exit=1`;
- `Test-Path` prints `True`, `False`;
- the generate log contains `vcpkg.exe not found`.

- [ ] **Step 3: A real Debug build**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\build.ps1 Debug; "exit=$LASTEXITCODE"
```

Expected:
- `build: Debug ok`, `exit=0`;
- `bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe` exists;
- `ReferenceProject\Binaries\ReferenceGame.dll` exists;
- all four logs exist.

Then `powershell -File scripts\run-tests.ps1 Debug --rng-seed 1` passes (the desk's `~[gpu]` baseline).

- [ ] **Step 4: Commit**

```bash
git add scripts/ci/build.ps1
git commit -F - <<'EOF'
ci: scripts/ci/build.ps1 -- the Windows build sequence moves out of ci.yml (generate, solution, ReferenceGame via arcbuild, solution again; stops at the first failing step; UTF-8 step logs)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 7: `scripts/ci/ci-image.sh` — publish-or-refuse, test-first

**Files:**
- Create: `scripts/ci/ci-image.sh` (100755)
- Test: `scripts/ci/tests/test-ci-image.sh` (100755)

**Interfaces:**
- Consumes:
  - `docker buildx imagetools inspect <ref> [--format '{{json .Manifest}}']`
  - `docker buildx build`
  - `ci/linux/verify-image.sh` (Task 3)
- Produces: `scripts/ci/ci-image.sh <image-repo> <tag>`, which reads `SAME_REPO` (`true` = may publish; anything else = fork).
  - Prints `ci-image: <ref> = sha256:<64 hex>`.
  - Appends `digest=sha256:<64 hex>` to `$GITHUB_OUTPUT` when that variable is set.
  - Exit 0, or 1 when the image is missing on a fork or the digest is unreadable. Task 9's `ci-image` job runs it.

- [ ] **Step 1: Write the failing test**

Create `scripts/ci/tests/test-ci-image.sh`:

```bash
#!/usr/bin/env bash
# Tests for scripts/ci/ci-image.sh with a stub `docker` on PATH (no daemon,
# no registry): a published tag is reused, a missing one is built + pushed +
# verified and pushed on a non-pull-request main run, verified locally
# without a push on a same-repo pull request, and refused -- never built --
# on a fork PR. The body below is the 2026-10-12 same-repo push step.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }
DIGEST=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

# STATE dir: calls.txt records every docker call; "published" exists once the
# image is in the (fake) registry. BAD_DIGEST=1 makes inspect print junk.
mkstub() {
    local d=$1
    mkdir -p "$d/bin"
    cat > "$d/bin/docker" <<EOF
#!/bin/sh
echo "docker \$*" >> "$d/calls.txt"
case "\$*" in
  "buildx imagetools inspect"*)
    [ -f "$d/published" ] || exit 1
    case "\$*" in *--format*)
      if [ -n "\${BAD_DIGEST:-}" ]; then echo '{"mediaType":"x"}'
      else echo '{"mediaType":"application/vnd.oci.image.manifest.v1+json","digest":"$DIGEST","size":1234}'; fi ;;
    esac
    exit 0 ;;
  "buildx build"*) touch "$d/published"; exit 0 ;;
  run*) exit 0 ;;
esac
exit 0
EOF
    chmod +x "$d/bin/docker"
}
run() {   # run <state> [VAR=value...]
    local d=$1; shift
    : > "$d/calls.txt"; : > "$d/out.txt"
    set +e
    env -u SAME_REPO -u GITHUB_OUTPUT PATH="$d/bin:$PATH" GITHUB_OUTPUT="$d/gh_output" "$@" \
        "$REPO/scripts/ci/ci-image.sh" ghcr.io/starworksdev/arcane-ci build-0123456789abcdef > "$d/out.txt" 2>&1
    RC=$?
    set -e
}

d=$(mktemp -d); mkstub "$d"; touch "$d/published"; run "$d" SAME_REPO=true
[ "$RC" -eq 0 ] && ! grep -q "buildx build" "$d/calls.txt" && grep -qx "digest=$DIGEST" "$d/gh_output" \
    && ok "published tag reused, digest output" || bad "published tag reused (rc $RC): $(cat "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=true
grep -q "buildx build --target build --provenance=false --push" "$d/calls.txt" \
    && grep -q -- "--build-context scripts=$REPO/scripts" "$d/calls.txt" \
    && grep -q "verify-image.sh" "$d/calls.txt" && [ "$RC" -eq 0 ] && grep -qx "digest=$DIGEST" "$d/gh_output" \
    && ok "missing tag built, pushed, verified" || bad "missing tag built (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=false
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" && grep -q "build-0123456789abcdef" "$d/out.txt" \
    && ok "fork PR refused, names the tag, builds nothing" || bad "fork PR refused (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d"
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" && ok "unset SAME_REPO counts as a fork" || bad "unset SAME_REPO counts as a fork (rc $RC)"

d=$(mktemp -d); mkstub "$d"; touch "$d/published"; run "$d" SAME_REPO=true BAD_DIGEST=1
[ "$RC" -eq 1 ] && [ ! -s "$d/gh_output" ] && ok "unreadable digest fails" || bad "unreadable digest fails (rc $RC)"

echo
[ "$fails" -eq 0 ] && echo "test-ci-image: all passed" || echo "test-ci-image: $fails failure(s)"
[ "$fails" -eq 0 ]
```

- [ ] **Step 2: Run it and watch it fail**

```bash
git add --chmod=+x scripts/ci/tests/test-ci-image.sh
docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash /r/scripts/ci/tests/test-ci-image.sh 2>&1 | tail -n 3
```

Expected: every case `FAIL` (`ci-image.sh: No such file`), then `test-ci-image: 5 failure(s)`, non-zero exit.

- [ ] **Step 3: Write `ci-image.sh`**

```bash
#!/usr/bin/env bash
# ci-image.sh <image-repo> <tag> -- the body of ci.yml's ci-image job: make
# sure <image-repo>:<tag> is published, then print its digest (the Linux legs
# run on image@digest, never on the tag).
#   published         -> reuse it
#   SAME_REPO=true    -> build it (ci/linux/Dockerfile, the scripts/ build
#                        context), push it, and run ci/linux/verify-image.sh in it
#   otherwise (a fork pull request, or SAME_REPO unset) -> fail, naming the tag:
#                        a fork cannot publish. The remedy runs fork-authored
#                        Dockerfile/script content in the one job holding
#                        packages: write, so a maintainer REVIEWS the ci/linux/
#                        and scripts/ diff first, then runs the workflow from a
#                        branch of this repository (docs/ci.md, "Image").
# Appends digest=<sha256:...> to $GITHUB_OUTPUT when set.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
image="${1:?usage: ci-image.sh <image-repo> <tag>}"
tag="${2:?usage: ci-image.sh <image-repo> <tag>}"
ref="$image:$tag"

if docker buildx imagetools inspect "$ref" > /dev/null 2>&1; then
    echo "ci-image: $ref is published"
elif [ "${SAME_REPO:-false}" = true ]; then
    echo "ci-image: $ref is not published -- building it"
    docker buildx build --target build --provenance=false --push \
        --build-context scripts="$REPO_ROOT/scripts" \
        -f "$REPO_ROOT/ci/linux/Dockerfile" -t "$ref" "$REPO_ROOT/ci/linux"
    docker run --rm -v "$REPO_ROOT/ci/linux:/v:ro" "$ref" bash /v/verify-image.sh
else
    echo "::error::$ref is not published, and a fork pull request cannot publish it. A maintainer reviews the ci/linux/ and scripts/ changes, then runs this workflow from a branch of this repository (docs/ci.md, Image)." >&2
    exit 1
fi

digest=$(docker buildx imagetools inspect "$ref" --format '{{json .Manifest}}' \
    | grep -o '"digest":"sha256:[0-9a-f]\{64\}"' | head -n 1 | cut -d'"' -f4 || true)
case "$digest" in
    sha256:*) ;;
    *) echo "::error::could not read the digest of $ref" >&2; exit 1 ;;
esac
echo "ci-image: $ref = $digest"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "digest=$digest" >> "$GITHUB_OUTPUT"; fi
```

- [ ] **Step 4: Run the tests and see them pass**

```bash
git add --chmod=+x scripts/ci/ci-image.sh
docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash /r/scripts/ci/tests/test-ci-image.sh 2>&1 | tail -n 7
```

Expected: five `ok` lines, then `test-ci-image: all passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/ci/tests/test-ci-image.sh
git commit -F - <<'EOF'
ci: scripts/ci/ci-image.sh -- reuse, or build + push + verify, the tagged Linux image and print its digest; a fork pull request is refused and never builds (spec s1.6, s2)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 8: The desk command — `rsync-filter.txt`, `container-leg.sh`, `local.ps1`

**Files:**
- Create: `scripts/ci/rsync-filter.txt`
- Create: `scripts/ci/container-leg.sh` (100755)
- Create: `scripts/ci/local.ps1`
- Modify: `scripts/ci/tests/test-build-sh.sh` (lint `container-leg.sh` too)
- No `.gitignore` change: `/logs/` already covers `logs/local/`.

**Interfaces:**
- Consumes: everything from Tasks 1, 3, 4 and 5.
- Produces:
  - `scripts\ci\local.ps1 [-Leg ...] [-Config ...] [-Full] [-Seeds 1|2] [-Seed N] [-Jobs N] [-NoPull]` → exit 0 when every leg passed, 1 when any leg failed, 2 on a preflight, image or tag failure.
  - Per leg, `logs\local\<leg>-<Config>\` holds:
    - `status.txt`;
    - `build-*.log`;
    - `tests-seed<N>.log`;
    - `test-results\`;
    - `container.log` (Linux legs) or `build.log` (Windows legs).
  - `container-leg.sh <Config> <seed[,seed]>` runs inside the image with `/src` (read-only), `/work` and `/out` mounted.

- [ ] **Step 1: Write the rsync filter**

Create `scripts/ci/rsync-filter.txt`:

```
# rsync filter for a local Linux leg: /src (the desk checkout) -> /work (the
# leg's volume), used by scripts/ci/container-leg.sh. First match wins.
# An EXCLUDED path is neither copied nor deleted (rsync --delete spares it),
# so /work keeps its own build outputs, generated Makefiles and tool links.
# Explicit list, not a .gitignore merge: ReferenceProject/Saved/verify-layout.ini
# is tracked yet matches .gitignore's *.ini.

# git metadata, at any depth: a desk worktree's (or a submodule's) .git FILE
# names a Windows path
- .git

# build outputs, reports, local junk (Windows and Linux)
- /bin/
- /bin-int/
- /.vs/
- /logs/
- /test-results/
- /Packages/
- /Intermediate/
- /.claude/
- /.superpowers/
- /graphify-out/
- /.example/
- /.screenshots/
- /vcpkg/
- /vcpkg_installed/
- /data/images/icons/
- /data/shaders/generated/
- node_modules/

# not part of the Linux engine build (Tauri app, its own workflow)
- /ArcaneHub/

# tools: Windows binaries never cross; build.sh links the image's own
- /ThirdParty/premake5/premake5
- /ThirdParty/premake5/premake5.exe
- /ThirdParty/tools/dxc/
- /ThirdParty/tools/dxc-linux
- /ThirdParty/tools/vulkan-sdk-macos/
- /ThirdParty/tools/dxc-macos/

# ReferenceProject outputs (Saved/ is ignored except the tracked layout seed)
- /ReferenceProject/Binaries/
- /ReferenceProject/Intermediate/
+ /ReferenceProject/Saved/verify-layout.ini
- /ReferenceProject/Saved/*

# generated project files (premake: vs2026 on Windows, gmake here)
- *.sln
- *.slnx
- *.vcxproj
- *.vcxproj.filters
- *.vcxproj.user
- /ThirdParty/*/ide-md/
- /Makefile
- /ReferenceProject/Makefile
- /Arcane*/Makefile
- /arcbuild/Makefile
- /arccook/Makefile
- /ArcaneTests/death-fixture/Makefile
- *.make
- *.ninja
- .ninja_log
- .ninja_deps

# object files, wherever a tool dropped them
- *.o
- *.obj
- *.pdb
- *.ilk
- *.pch
- *.gch
```

- [ ] **Step 2: Write `container-leg.sh`**

```bash
#!/usr/bin/env bash
# container-leg.sh <Config> <seed[,seed]> -- ONE local Linux leg inside the
# arcane-ci image, started by scripts/ci/local.ps1 (docs/ci.md, "Local runs").
#   /src   the desk checkout, read-only
#   /work  this leg's named volume (arcane-src-<leg>): incremental builds
#   /out   logs\local\<leg>-<Config>\ on the desk
# 1. mirror /src -> /work (rsync --delete, scripts/ci/rsync-filter.txt)
# 2. scripts/ci/build.sh <Config> -j $MAKE_JOBS
# 3. per seed: scripts/run-tests.sh under Xvfb with SDL's x11 driver, as CI
# 4. copy logs/* and test-results/ to /out; key=value results to /out/status.txt
#    (sync=, sync_seconds=, build=, tests-<seed>=)
# Exit 0 when the build and every seed passed.
set -uo pipefail
CONFIG="${1:?usage: container-leg.sh <Config> <seed[,seed]>}"
SEEDS="${2:?usage: container-leg.sh <Config> <seed[,seed]>}"
OUT=/out
STATUS="$OUT/status.txt"
: > "$STATUS"
note() { echo "$1=$2" >> "$STATUS"; }

t0=$(date +%s)
if ! rsync -a --delete --filter="merge /src/scripts/ci/rsync-filter.txt" /src/ /work/; then
    echo "container-leg: rsync /src -> /work failed" >&2
    note sync 1
    exit 1
fi
note sync 0
note sync_seconds $(( $(date +%s) - t0 ))
echo "container-leg: synced /src -> /work in $(( $(date +%s) - t0 ))s"

cd /work || exit 1
rm -rf logs test-results
scripts/ci/build.sh "$CONFIG" -j "${MAKE_JOBS:-2}"
build=$?
note build "$build"
# An out-of-memory VM kills cc1plus/ld, not the container: make exits 2. The
# kill shows up in the step logs; only then is the failure called OOM.
if [ "$build" -ne 0 ] && grep -qE "Killed signal terminated program|terminated with signal 9|signal 9 \(Killed\)" logs/build-*.log 2> /dev/null; then
    note oom 1
fi
failed=$build
if [ "$build" -eq 0 ]; then
    export SDL_VIDEODRIVER=x11
    for seed in $(echo "$SEEDS" | tr ',' ' '); do
        xvfb-run -a -s "-screen 0 1920x1080x24" scripts/run-tests.sh "$CONFIG" --rng-seed "$seed" \
            > "logs/tests-seed$seed.log" 2>&1
        rc=$?
        note "tests-$seed" "$rc"
        echo "container-leg: seed $seed -> exit $rc"
        tail -n 4 "logs/tests-seed$seed.log"
        [ "$rc" -eq 0 ] || failed=1
    done
fi
cp logs/* "$OUT/" 2> /dev/null || true
if [ -d test-results ]; then cp -R test-results "$OUT/"; fi
exit "$failed"
```

Add it to the bash-3.2 lint in `scripts/ci/tests/test-build-sh.sh`: replace

```bash
LINTED="$REPO/scripts/ci/build.sh"
```

with

```bash
LINTED="$REPO/scripts/ci/build.sh $REPO/scripts/ci/container-leg.sh"
```

- [ ] **Step 3: Write `local.ps1`**

```powershell
# local.ps1 -- run Arcane's CI legs on this desk before pushing (docs/ci.md,
# "Local runs"). One-time setup: scripts\ci\setup-desk.ps1.
#
#   scripts\ci\local.ps1                          # windows-msvc Debug + linux-gcc-14 Debug
#   scripts\ci\local.ps1 -Leg linux-clang-19 -Config Release
#   scripts\ci\local.ps1 -Full                    # windows-msvc, linux-gcc-14, linux-clang-19 x Debug/Release/Dist
#        [-Seeds 1|2] [-Seed N] [-Jobs N] [-NoPull]
#
# Windows legs: scripts\ci\build.ps1 + scripts\run-tests.ps1, natively, in
# this checkout. Linux legs: scripts/ci/build.sh + scripts/run-tests.sh in the
# CI image ghcr.io/starworksdev/arcane-ci:<scripts\ci\image-tag.ps1> -- pulled,
# else (or with -NoPull) built here and labelled "<tag>-local (local image)".
# The checkout is mounted read-only and mirrored into a volume per leg
# (arcane-src-<leg>); compiles go through ccache (volume arcane-ccache).
# Uncommitted changes are included -- it is a pre-push check.
#
# Legs run one at a time, Windows first, and every requested leg runs even
# when one fails. Seeds: one random seed (printed) by default; -Seeds 2 runs
# CI's pair (N, (N + 50021) mod 4294967296); -Seed N fixes N (CI's N is the
# run number). Jobs:
# Linux = min(CPUs, memory GB / 2) of what Docker reports, Windows = msbuild
# /m; -Jobs N sets both. Logs: logs\local\<leg>-<Config>\.
# Exit: 0 every leg passed; 1 a leg failed; 2 preflight / image / tag failure.
param(
    [string[]]$Leg,
    [string[]]$Config,
    [switch]$Full,
    [ValidateSet(1, 2)][int]$Seeds = 1,
    [string]$Seed,
    [int]$Jobs = 0,
    [switch]$NoPull
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$imageRepo = 'ghcr.io/starworksdev/arcane-ci'
$localRoot = Join-Path $root 'logs\local'

# `powershell -File local.ps1 -Leg a,b` passes ONE string "a,b": split it.
if ($Leg) { $Leg = @($Leg | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
if ($Config) { $Config = @($Config | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
try {
    $runs = Resolve-LocalRuns -Leg $Leg -Config $Config -Full:$Full
    if (-not $Seed) { $Seed = [string](Get-Random -Minimum 1 -Maximum 42950) }
    $seedList = Get-SeedList -Seed $Seed -Seeds $Seeds
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 2
}
$needLinux = @($runs | Where-Object { $_.Kind -eq 'linux' }).Count -gt 0
$needWindows = @($runs | Where-Object { $_.Kind -eq 'windows' }).Count -gt 0
Write-Host ('local: {0} leg(s): {1}; seed(s) {2}' -f $runs.Count, (($runs | ForEach-Object { "$($_.Leg) $($_.Config)" }) -join ', '), ($seedList -join ', '))

# ---- preflight: every failed check names its fix; nothing builds before it passes
$problems = @()
$dockerCpus = 0
$dockerMem = [long]0
if ($needLinux) {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        $problems += 'docker not found: install Docker Desktop (WSL2 backend), then run scripts\ci\setup-desk.ps1'
    } else {
        $info = Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('info', '--format', '{{.OSType}} {{.NCPU}} {{.MemTotal}}')
        if ($info.Code -ne 0 -or -not $info.Output) {
            $problems += 'Docker is not running: start Docker Desktop'
        } else {
            $parts = ("$($info.Output[0])").Trim() -split '\s+'
            if ($parts[0] -ne 'linux') { $problems += "Docker is in '$($parts[0])' container mode: switch Docker Desktop to Linux containers" }
            else { $dockerCpus = [int]$parts[1]; $dockerMem = [long]$parts[2] }
        }
    }
    $wsl = Join-Path $env:USERPROFILE '.wslconfig'
    $wslText = ''
    if (Test-Path -LiteralPath $wsl) { $wslText = [System.IO.File]::ReadAllText($wsl) }
    if (-not (Test-WslConfigLimits -Text $wslText)) {
        Write-Host "local: WARNING $wsl is below 24 GB / 16 processors -- Linux legs will be slow or run out of memory. Run scripts\ci\setup-desk.ps1, then wsl --shutdown." -ForegroundColor Yellow
    }
}
if ($needWindows) {
    try { $null = Resolve-MSBuild } catch { $problems += "$($_.Exception.Message) -- install Visual Studio 2026 with Desktop development with C++" }
}
if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host "local: $p" -ForegroundColor Red }
    exit 2
}

$linuxJobs = 0
if ($Jobs -gt 0) { $linuxJobs = $Jobs } elseif ($needLinux) { $linuxJobs = Get-LinuxJobCount -Cpus $dockerCpus -MemoryBytes $dockerMem }
New-Item -ItemType Directory -Force -Path $localRoot | Out-Null

# ---- the Linux image: pull the published tag, else build it here as <tag>-local
$image = $null
if ($needLinux) {
    $tag = & (Join-Path $PSScriptRoot 'image-tag.ps1')
    if ($LASTEXITCODE -ne 0 -or -not $tag) { Write-Host 'local: scripts\ci\image-tag.ps1 refused the image inputs (see above)' -ForegroundColor Red; exit 2 }
    $remote = "${imageRepo}:$tag"
    if (-not $NoPull) {
        $code = Invoke-NativeLogged -FilePath 'docker' -ArgumentList @('pull', $remote) -LogPath (Join-Path $localRoot 'image-pull.log')
        if ($code -eq 0) { $image = [pscustomobject]@{ Ref = $remote; Label = $tag } }
        else { Write-Host "local: $remote could not be pulled (exit $code) -- using a local build of the same Dockerfile" -ForegroundColor Yellow }
    }
    if (-not $image) {
        $localRef = "arcane-ci:$tag-local"
        if ((Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('image', 'inspect', $localRef)).Code -ne 0) {
            $code = Invoke-NativeLogged -FilePath 'docker' -ArgumentList @('build', '--target', 'build',
                '--build-context', "scripts=$(Join-Path $root 'scripts')",
                '-f', (Join-Path $root 'ci\linux\Dockerfile'), '-t', $localRef, (Join-Path $root 'ci\linux')) `
                -LogPath (Join-Path $localRoot 'image-build.log')
            if ($code -ne 0) { Write-Host "local: building $localRef failed (exit $code) -- logs\local\image-build.log" -ForegroundColor Red; exit 2 }
        }
        $image = [pscustomobject]@{ Ref = $localRef; Label = "$tag-local (local image)" }
    }
    Write-Host "local: Linux image $($image.Label), $linuxJobs job(s)"
}

function New-LegOutDir($Run) {
    $dir = Join-Path $localRoot "$($Run.Leg)-$($Run.Config)"
    if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

function Invoke-WindowsLeg($Run) {
    $out = New-LegOutDir $Run
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $status = [ordered]@{}
    $env:ARCANE_CI_LEG = $Run.Leg
    $buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'build.ps1'), $Run.Config)
    if ($Jobs -gt 0) { $buildArgs += @('-Jobs', [string]$Jobs) }
    $status['build'] = Invoke-NativeLogged -FilePath 'powershell.exe' -ArgumentList $buildArgs -LogPath (Join-Path $out 'build.log')
    if ($status['build'] -eq 0) {
        # Earlier runs' results for this leg/config (other seeds) must not ride along.
        Remove-Item -Path (Join-Path $root "test-results\$($Run.Leg)-$($Run.Config)-seed*") -ErrorAction SilentlyContinue
        foreach ($s in $seedList) {
            $status["tests-$s"] = Invoke-NativeLogged -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', (Join-Path $root 'scripts\run-tests.ps1'), $Run.Config, '--rng-seed', $s) -LogPath (Join-Path $out "tests-seed$s.log")
        }
    }
    Copy-Item -Path (Join-Path $root 'logs\build-*.log') -Destination $out -ErrorAction SilentlyContinue
    $results = Join-Path $root 'test-results'
    if (Test-Path -LiteralPath $results) {
        $dest = Join-Path $out 'test-results'
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Get-ChildItem -LiteralPath $results -Filter "$($Run.Leg)-$($Run.Config)-seed*" | Copy-Item -Destination $dest
    }
    Write-LegStatus -Path (Join-Path $out 'status.txt') -Status $status
    return New-LegResult -Run $Run -Status $status -OutDir $out -Seeds $seedList -Seconds $sw.Elapsed.TotalSeconds -Image 'native (this checkout)'
}

function Invoke-LinuxLeg($Run) {
    $out = New-LegOutDir $Run
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $name = "arcane-local-$($Run.Leg)-$($Run.Config.ToLowerInvariant())"
    $null = Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('rm', '-f', $name)
    $dockerArgs = @('run', '--rm', '--init', '--shm-size=2g', '--user', '1000:1000', '--name', $name,
        '-v', "${root}:/src:ro", '-v', "arcane-src-$($Run.Leg):/work", '-v', 'arcane-ccache:/ccache', '-v', "${out}:/out",
        '-e', "CC=$($Run.CC)", '-e', "CXX=$($Run.CXX)", '-e', "ARCANE_CI_LEG=$($Run.Leg)", '-e', "MAKE_JOBS=$linuxJobs",
        '-e', 'CCACHE_DIR=/ccache', '-e', 'CCACHE_MAXSIZE=20G', '-e', 'TINI_KILL_PROCESS_GROUP=1',
        '-e', 'PATH=/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
        $image.Ref, 'bash', '/src/scripts/ci/container-leg.sh', $Run.Config, ($seedList -join ','))
    $code = -1
    try {
        $code = Invoke-NativeLogged -FilePath 'docker' -ArgumentList $dockerArgs -LogPath (Join-Path $out 'container.log')
    } finally {
        $null = Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('rm', '-f', $name)
    }
    $status = Read-LegStatus -Path (Join-Path $out 'status.txt')
    if (-not $status.Contains('build')) { $status['container'] = $code }
    return New-LegResult -Run $Run -Status $status -OutDir $out -Seeds $seedList -Seconds $sw.Elapsed.TotalSeconds -Image $image.Label
}

$results = @()
foreach ($run in $runs) {
    Write-Host ''
    Write-Host "==== local: $($run.Leg) $($run.Config) ====" -ForegroundColor Cyan
    if ($run.Kind -eq 'windows') { $results += Invoke-WindowsLeg $run } else { $results += Invoke-LinuxLeg $run }
}
Format-LocalSummary -Results $results | ForEach-Object { Write-Host $_ }
if (@($results | Where-Object { -not $_.Passed }).Count -gt 0) { exit 1 }
exit 0
```

- [ ] **Step 4: The first Linux leg, measuring the sync**

```bash
git add --chmod=+x scripts/ci/container-leg.sh
git add scripts/ci/rsync-filter.txt scripts/ci/local.ps1 scripts/ci/tests/test-build-sh.sh
```

Then, in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\local.ps1 -Leg linux-gcc-14 -Config Debug -Seed 1; "exit=$LASTEXITCODE"
```

Expected:
- `local: Linux image build-<hash>-local (local image), 11 job(s)` (or 12). Nothing is published yet, so the pull fails and the image builds or reuses `arcane-ci:<tag>-local`.
- A `PASS linux-gcc-14 Debug` row, `tests: seed 1: <P>/<C> cases...`, `exit=0`.

Record `sync_seconds` from `logs\local\linux-gcc-14-Debug\status.txt` (first sync, cold). Rerun the same command with no changes and record `sync_seconds` again (the no-change sync, spec s1.4). If the no-change sync is above 30 s:
1. Run `docker run --rm -v "${PWD}:/src:ro" <image> du -sh /src/* | sort -h` to find the largest trees.
2. Add filter lines for any the Linux build never reads.
3. Rerun, and record both numbers in the commit message.

Also re-run `docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash /r/scripts/ci/tests/test-build-sh.sh | tail -n 3` (the new lint line), expecting `all passed`.

- [ ] **Step 5: Review Focus 5, the unpublished image is labelled**

The Step 4 run already shows it: the summary's `image:` line reads `build-<hash>-local (local image)` and `logs\local\image-pull.log` holds the pull failure.

Run `powershell -File scripts\ci\local.ps1 -Leg linux-gcc-14 -NoPull`. The same label appears, and no `image-pull.log` is rewritten (its timestamp is unchanged).

- [ ] **Step 6: Review Focus 1, a worktree after a Windows build mirrors cleanly**

The worktree now has `bin\Debug-windows-x86_64-md\` and `*.vcxproj` from Task 6. Inspect the leg's volume:

```bash
docker run --rm -v arcane-src-linux-gcc-14:/work arcane-ci:dev sh -c 'ls -a /work | head -50; ls /work/bin; test -e /work/.git && echo HAS-GIT; ls /work/*.vcxproj 2>/dev/null | head -2; ls -l /work/ThirdParty/premake5/ /work/ThirdParty/tools/'
```

Expected:
- no `HAS-GIT`;
- `/work/bin` holds only `*-linux-x86_64-md` directories;
- no `.vcxproj`;
- `ThirdParty/premake5/premake5` is a symlink to `/opt/arcane-tools/...`;
- `ThirdParty/tools/` has a `dxc-linux` symlink and no Windows `dxc/`;
- `ReferenceProject/Saved/verify-layout.ini` exists.

Then touch one engine `.cpp` on the desk and rerun Step 4's command. The leg relinks quickly (minutes, not a cold build), proving `--delete` spared the outputs.

- [ ] **Step 7: Review Focus 3, the preflight stops early**

Quit Docker Desktop, then run:

```powershell
powershell -File scripts\ci\local.ps1 -Leg linux-gcc-14; "exit=$LASTEXITCODE"
```

Expected: `local: Docker is not running: start Docker Desktop` in red, `exit=2`, and no new `logs\local\linux-gcc-14-Debug`. Restart Docker.

Temporarily copy `.wslconfig` aside and write the old 8 GB content, then run the same command. Expected: the yellow WARNING line, and the run proceeds. Ctrl-C it once the build starts. Restore `.wslconfig`.

- [ ] **Step 8: Review Focus 2, Ctrl-C leaves no container and keeps the volumes**

Start `powershell -File scripts\ci\local.ps1 -Leg linux-gcc-14`. Press Ctrl-C during the build step, then run:

```powershell
docker ps -a --filter name=arcane-local- --format '{{.Names}}'   # expected: nothing
docker volume ls --format '{{.Name}}' | Select-String arcane-   # expected: arcane-ccache, arcane-src-linux-gcc-14
```

Rerun the leg. It resumes incrementally and passes.

- [ ] **Step 9: The default run (spec s1.4)**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\local.ps1; "exit=$LASTEXITCODE"
```

Expected: two rows, `PASS windows-msvc Debug` and `PASS linux-gcc-14 Debug`, then `2 of 2 leg(s) passed`, `exit=0`. Windows runs first.

- [ ] **Step 10: Commit**

```bash
git status --short          # logs/ must not appear (gitignored /logs/)
git commit -F - <<'EOF'
ci: scripts/ci/local.ps1 -- the desk's one CI command: Windows legs natively, Linux legs in the CI image (pulled, else built and labelled <tag>-local) over a read-only checkout mirrored per leg (rsync-filter.txt, container-leg.sh), ccache, serial legs, preflight, one pass/fail summary

no-change sync: <N> s (first sync <M> s)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

Replace `<N>`/`<M>` with the numbers measured in Step 4. Do not commit the placeholders.

---

### Task 9: `.github/workflows/ci.yml` — the scripts become the workflow

**Files:**
- Modify (full rewrite): `.github/workflows/ci.yml`

**Interfaces:**
- Consumes:
  - `scripts/ci/tests/test-{image-tag,build-sh,ci-image}.sh`
  - `scripts/ci/tests/test-arcane-ci.ps1`
  - `scripts/ci/image-tag.sh`, `scripts/ci/ci-image.sh`
  - `scripts/ci/build.sh`, `scripts/ci/build.ps1`
  - `scripts/run-tests.{sh,ps1}`, `scripts/linux-windowed-smoke.sh`
- Produces:
  - Job ids `image-tag`, `ci-image`, `build-test-linux`, `build-test`, `notify`.
  - Display names `<leg> <Config>`, unchanged.
  - Artifact names `test-results-<leg>-<Config>` and `logs-<leg>-<Config>`, unchanged.

- [ ] **Step 1: Replace the file**

Write `.github/workflows/ci.yml`:

```yaml
name: CI

# The ONE hosted CI workflow for Arcane: a matrix over
# {windows-msvc, linux-gcc-14, linux-clang-19, macos-appleclang} x
# {Debug, Release, Dist}. The build and test sequences live in scripts, not
# here -- this file and scripts/ci/local.ps1 (the desk) are two callers:
#   build  scripts/ci/build.sh (Linux, macOS) / scripts/ci/build.ps1 (Windows)
#   test   scripts/run-tests.sh / scripts/run-tests.ps1, two seeds per leg
# docs/ci.md is the standard this file implements (shared with Astra and
# Manifold2D) and records its deviations.
#
# Jobs:
#   image-tag         the CI scripts' own tests, then the Linux image tag
#                     (scripts/ci/image-tag.sh: a hash of ci/linux/Dockerfile
#                     and the repo files it copies)
#   ci-image          ghcr.io/starworksdev/arcane-ci:<tag> -- reused, or built,
#                     verified, and pushed only on a non-pull-request run of
#                     main (scripts/ci/ci-image.sh; the ONLY job with
#                     packages: write). A pull request with a new tag verifies
#                     locally and outputs an empty digest
#   build-test-linux  the Linux legs, inside that image pinned by digest.
#                     Skipped when the digest is empty
#   build-test        the Windows and macOS legs, on the hosted runners
#   notify            Discord FAILED / RECOVERED, never for pull requests
#
# Tests run "~[gpu]" minus the per-OS exclusion list on every leg. Extra lanes
# ride ONE leg each, never a separate workflow:
#   * linux-gcc-14 Debug: [gpu] on Mesa lavapipe with the image's pinned
#     validation layer, and the X11 + Wayland windowed smoke
#     (scripts/linux-windowed-smoke.sh);
#   * macos-appleclang Debug: [gpu] on MoltenVK over the runner's Apple GPU
#     (see docs/ci.md "GPU lanes" for what that proves and what it cannot).
# The Jenkins full gate on the GPU desk (Jenkinsfile) stays the unfiltered,
# real-GPU, golden-gate run until it is retired (sub-project B).

on:
  push:
    branches: [main]
  pull_request:
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

env:
  # The Linux legs' image (ci/linux/Dockerfile; docs/ci.md "Image").
  CI_IMAGE: ghcr.io/starworksdev/arcane-ci
  # Xcode is pinned, never "whatever the image defaults to" (docs/ci.md).
  XCODE_APP: /Applications/Xcode_16.4.app

jobs:
  image-tag:
    name: CI scripts + image tag
    runs-on: ubuntu-24.04
    timeout-minutes: 10
    outputs:
      tag: ${{ steps.tag.outputs.tag }}
    defaults:
      run:
        shell: bash
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false

      - name: CI script tests
        run: |
          scripts/ci/tests/test-image-tag.sh
          scripts/ci/tests/test-build-sh.sh
          scripts/ci/tests/test-ci-image.sh
          pwsh -NoProfile -File scripts/ci/tests/test-arcane-ci.ps1

      - name: Image tag
        id: tag
        run: |
          tag=$(scripts/ci/image-tag.sh)
          echo "tag=$tag" >> "$GITHUB_OUTPUT"
          echo "Linux CI image tag: $tag"

  ci-image:
    name: CI image
    needs: image-tag
    runs-on: ubuntu-24.04
    timeout-minutes: 90
    permissions:
      contents: read
      packages: write
    # Two runs that miss the same tag build it once: the second waits, then
    # finds it published.
    concurrency:
      group: arcane-ci-image-${{ needs.image-tag.outputs.tag }}
      cancel-in-progress: false
    outputs:
      tag: ${{ needs.image-tag.outputs.tag }}
      digest: ${{ steps.image.outputs.digest }}
    env:
      TAG: ${{ needs.image-tag.outputs.tag }}
      # A fork pull request never publishes (scripts/ci/ci-image.sh), and
      # neither does Dependabot: its same-repo PRs get a read-only token, so a
      # build would run ~30 min and then fail at the push.
      SAME_REPO: ${{ (github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository) && github.actor != 'dependabot[bot]' }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false

      - uses: docker/login-action@dbcb813823bdd20940b903addbd779551569679f # v4.6.0
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ github.token }}

      - uses: docker/setup-buildx-action@f87e5991a6d7451dcb8d9637bfbc97413f497069 # v4.4.1

      - name: Resolve or build the image
        id: image
        shell: bash
        run: scripts/ci/ci-image.sh "$CI_IMAGE" "$TAG"

  build-test-linux:
    name: ${{ matrix.leg }} ${{ matrix.config }}
    needs: ci-image
    runs-on: ubuntu-24.04
    timeout-minutes: 150
    permissions:
      contents: read
      packages: read
    # The image by its immutable digest, never the tag. credentials: the
    # workflow's own token (read-only on fork PRs), so the legs pull even
    # before the package is made public (docs/ci.md, Image).
    container:
      image: ghcr.io/starworksdev/arcane-ci@${{ needs.ci-image.outputs.digest }}
      credentials:
        username: ${{ github.actor }}
        password: ${{ github.token }}
      options: --init --shm-size=2g
    strategy:
      fail-fast: false
      matrix:
        leg: [linux-gcc-14, linux-clang-19]
        config: [Debug, Release, Dist]
        include:
          - leg: linux-gcc-14
            cc: gcc-14
            cxx: g++-14
          - leg: linux-clang-19
            cc: clang-19
            cxx: clang++-19
    env:
      ARCANE_CI_LEG: ${{ matrix.leg }}
      CC: ${{ matrix.cc }}
      CXX: ${{ matrix.cxx }}
      # Two seeds per leg (ArcaneTests runs in RANDOM order). Both are printed
      # in the suite banner and in each JUnit file name.
      # 2026-10-12 step below concatenates. Current SEED_B is
      # (run_number + 50021) mod 4294967296, computed in bash (docs/ci.md).
      SEED_A: ${{ github.run_number }}
      SEED_B: ${{ github.run_number }}50021
      LINUX_GPU_LANE: ${{ matrix.leg == 'linux-gcc-14' && matrix.config == 'Debug' }}
      # Hosted Linux runners are 4 vCPU / 16 GB and the Debug ArcaneTests link
      # alone is ~500 MB: -j3 bounds peak memory.
      MAKE_JOBS: 3
    defaults:
      run:
        shell: bash
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false

      - name: Toolchain (CI image)
        run: |
          echo "image ${{ needs.ci-image.outputs.tag }} @ ${{ needs.ci-image.outputs.digest }}"
          "$CXX" --version
          echo "SDL3 $(pkg-config --modversion sdl3)"

      - name: Build
        run: scripts/ci/build.sh ${{ matrix.config }} -j "$MAKE_JOBS"

      # Under Xvfb with SDL's x11 driver, so windows are real native windows.
      - name: Test (two seeds)
        env:
          SDL_VIDEODRIVER: x11
        run: |
          mkdir -p logs
          failed=0
          for seed in "$SEED_A" "$SEED_B"; do
            if ! xvfb-run -a -s "-screen 0 1920x1080x24" scripts/run-tests.sh ${{ matrix.config }} --rng-seed "$seed" \
                > "logs/tests-seed$seed.log" 2>&1; then
              echo "::error::seed $seed failed"
              failed=1
              # Every failing case's block, so the job log alone diagnoses
              # it (the artifacts are not always reachable).
              echo "::group::failures, seed $seed"
              grep -B10 -A20 -E "FAILED:|due to unexpected exception|due to a fatal error" "logs/tests-seed$seed.log" | head -n 4000 || true
              echo "::endgroup::"
            fi
            tail -n 60 "logs/tests-seed$seed.log"
          done
          exit $failed

      - name: Test [gpu] (Linux, lavapipe)
        if: env.LINUX_GPU_LANE == 'true'
        env:
          SDL_VIDEODRIVER: x11
          VK_DRIVER_FILES: /usr/share/vulkan/icd.d/lvp_icd.json
          # The bare runner got /run/user/1001 from systemd-logind; in the
          # container nothing sets it. The image creates root's (0700). Not
          # in the Dockerfile ENV: local runs are uid 1000.
          XDG_RUNTIME_DIR: /run/user/0
        run: |
          set -o pipefail
          xvfb-run -a -s "-screen 0 1920x1080x24" scripts/run-tests.sh --gpu ${{ matrix.config }} \
            --rng-seed "$SEED_A" 2>&1 | tee logs/tests-gpu.log

      - name: Windowed smoke (Linux, X11 + Wayland)
        if: env.LINUX_GPU_LANE == 'true'
        env:
          VK_DRIVER_FILES: /usr/share/vulkan/icd.d/lvp_icd.json
          # x11 hosts use it; the Wayland half makes its own (the smoke's
          # weston gets a private mktemp runtime dir).
          XDG_RUNTIME_DIR: /run/user/0
        run: |
          set -o pipefail
          xvfb-run -a -s "-screen 0 1920x1080x24" scripts/linux-windowed-smoke.sh \
            ${{ matrix.config }} logs/windowed x11 wayland 2>&1 | tee logs/windowed-smoke.log

      # Every distinct compiler/linker error of the build in one place (the
      # build runs `make -k`), so a red leg reads at a glance.
      - name: Summarize build errors
        if: failure()
        run: |
          grep -hE "(error|undefined reference|Undefined symbols|ld: )" logs/build-*.log 2>/dev/null | sort -u | head -150 || true

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: test-results-${{ matrix.leg }}-${{ matrix.config }}
          path: test-results/
          if-no-files-found: ignore
          retention-days: 14

      # Crashes/hangs drop symbolized stacks under <exe dir>/diagnostics/.
      - name: Upload logs
        if: failure() || env.LINUX_GPU_LANE == 'true'
        uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: logs-${{ matrix.leg }}-${{ matrix.config }}
          path: |
            logs/
            bin/*/ArcaneTests/diagnostics/
            bin/*/ArcaneTests/*.log
            bin/*/ArcaneTests/ReferenceProject/Saved/Verify/
            /tmp/arcane-witness/
          if-no-files-found: ignore
          retention-days: 14

  build-test:
    name: ${{ matrix.leg }} ${{ matrix.config }}
    runs-on: ${{ matrix.os }}
    # macOS gets a longer budget: its runner builds with fewer jobs (MAKE_JOBS).
    timeout-minutes: ${{ matrix.timeout || 150 }}
    strategy:
      fail-fast: false
      matrix:
        leg: [windows-msvc, macos-appleclang]
        config: [Debug, Release, Dist]
        include:
          - leg: windows-msvc
            os: windows-2025
            compiler: msvc
          - leg: macos-appleclang
            os: macos-15
            compiler: appleclang
            cc: clang
            cxx: clang++
            make_jobs: 2
            timeout: 300
    env:
      ARCANE_CI_LEG: ${{ matrix.leg }}
      CC: ${{ matrix.cc }}
      CXX: ${{ matrix.cxx }}
      # 2026-10-12 step below concatenates. Current SEED_B is
      # (run_number + 50021) mod 4294967296, computed in bash (docs/ci.md).
      SEED_A: ${{ github.run_number }}
      SEED_B: ${{ github.run_number }}50021
      MACOS_GPU_LANE: ${{ matrix.leg == 'macos-appleclang' && matrix.config == 'Debug' }}
      # macos-15 arm64 runners are 3 vCPU / 7 GB, and since the settings arc
      # the heaviest editor/test TUs peak at 1.4-1.9 GB each: three at once
      # swap the VM (a pre-merge macOS build took 51 minutes; CI 38076751748
      # hit the 150-minute cap mid-build), so macOS builds with -j2 under a
      # longer timeout.
      MAKE_JOBS: ${{ matrix.make_jobs || 3 }}
    defaults:
      run:
        shell: bash
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false

      # ------------------------------------------------------------------
      # Toolchains
      # ------------------------------------------------------------------
      - name: Toolchain (Windows MSVC)
        if: runner.os == 'Windows'
        uses: microsoft/setup-msbuild@30375c66a4eea26614e0d39710365f22f8b0af57 # v3

      - name: Toolchain (macOS Apple Clang, pinned Xcode)
        if: runner.os == 'macOS'
        run: |
          sudo xcode-select -s "$XCODE_APP"
          xcodebuild -version
          "$CXX" --version
          sw_vers
          sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize

      # ------------------------------------------------------------------
      # Third-party dependencies (every download version-pinned + SHA-256)
      # ------------------------------------------------------------------
      # SDL3 builds from source under the repo's v143 static-md overlay
      # triplet (~10 min) -- cache the installed tree.
      - name: Cache SDL3 (Windows, vcpkg)
        if: runner.os == 'Windows'
        uses: actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0
        with:
          path: C:\vcpkg\installed
          # One key per image, not per config: the installed tree serves every
          # configuration (audit C9 -- per-config keys stored identical copies).
          key: ${{ matrix.os }}-vcpkg-${{ hashFiles('vcpkg-triplets/**', 'scripts/setup-vcpkg-deps.bat') }}
          restore-keys: ${{ matrix.os }}-vcpkg-

      - name: SDL3 (Windows, vcpkg overlay triplet)
        if: runner.os == 'Windows'
        shell: cmd
        run: |
          set _APH_NOPAUSE=1
          call scripts\setup-vcpkg-deps.bat

      # macOS does not package SDL3: scripts/build-sdl3.sh builds the pinned
      # release once into a cached prefix, copied into /usr/local
      # (premake5.lua gives libArcaneClient.dylib an rpath to it).
      - name: Cache SDL3 (macOS)
        if: runner.os == 'macOS'
        uses: actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0
        with:
          path: ~/sdl3-prefix
          # SDL3 always builds Release: one copy per image (audit C9).
          key: ${{ matrix.os }}-sdl3-${{ hashFiles('scripts/build-sdl3.sh') }}

      - name: SDL3 (macOS, from source)
        if: runner.os == 'macOS'
        run: |
          if [ ! -f "$HOME/sdl3-prefix/include/SDL3/SDL.h" ]; then
            scripts/build-sdl3.sh "$HOME/sdl3-prefix"
          fi
          sudo mkdir -p /usr/local
          sudo cp -R "$HOME/sdl3-prefix/." /usr/local/
          echo "SDL3_ROOT=/usr/local" >> "$GITHUB_ENV"
          ls /usr/local/lib | grep -i sdl3

      # premake lands where Arcane::Toolchain looks for a bundled copy on
      # POSIX (ThirdParty/premake5/premake5, beside the vendored .exe).
      - name: Cache premake + Vulkan SDK (macOS)
        if: runner.os == 'macOS'
        uses: actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0
        with:
          path: |
            ThirdParty/premake5/premake5
            ThirdParty/tools/vulkan-sdk-macos/download
          # Identical downloads for every config: one copy per image (audit C9;
          # the per-config keys held fifteen 326 MB copies on 2026-10-12).
          key: ${{ matrix.os }}-tools-${{ hashFiles('scripts/fetch-premake.sh', 'scripts/fetch-vulkan-sdk-macos.sh') }}

      # MoltenVK + the Vulkan loader + validation layer + DXC, one pinned SDK
      # (the script header says why this is the macOS DXC source).
      - name: premake + Vulkan SDK (MoltenVK, DXC) (macOS)
        if: runner.os == 'macOS'
        run: |
          scripts/fetch-premake.sh
          scripts/fetch-vulkan-sdk-macos.sh --system
          # MoltenVK is the ONLY ICD the leg may see.
          echo "VK_DRIVER_FILES=/usr/local/share/vulkan/icd.d/MoltenVK_icd.json" >> "$GITHUB_ENV"

      # ------------------------------------------------------------------
      # Build: the shared scripts (docs/ci.md, "Build driver")
      # ------------------------------------------------------------------
      - name: Build (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: ./scripts/ci/build.ps1 ${{ matrix.config }}

      - name: Build (macOS)
        if: runner.os == 'macOS'
        run: scripts/ci/build.sh ${{ matrix.config }} -j "$MAKE_JOBS"

      # ------------------------------------------------------------------
      # Test: "~[gpu]" minus the per-OS exclusions, two seeds
      # ------------------------------------------------------------------
      - name: Test (Windows, two seeds)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          $failed = 0
          foreach ($seed in @($env:SEED_A, $env:SEED_B)) {
            powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-tests.ps1 ${{ matrix.config }} --rng-seed $seed
            if ($LASTEXITCODE -ne 0) { Write-Host "::error::seed $seed failed ($LASTEXITCODE)"; $failed = 1 }
          }
          exit $failed

      # SDL's cocoa driver in the runner's Aqua session, so windows are real
      # NSWindows with a CAMetalLayer (offscreen has none).
      - name: Test (macOS, two seeds)
        if: runner.os == 'macOS'
        env:
          SDL_VIDEODRIVER: cocoa
        run: |
          mkdir -p logs
          failed=0
          for seed in "$SEED_A" "$SEED_B"; do
            if ! scripts/run-tests.sh ${{ matrix.config }} --rng-seed "$seed" > "logs/tests-seed$seed.log" 2>&1; then
              echo "::error::seed $seed failed"
              failed=1
              echo "::group::failures, seed $seed"
              grep -B10 -A20 -E "FAILED:|due to unexpected exception|due to a fatal error" "logs/tests-seed$seed.log" | head -n 4000 || true
              echo "::endgroup::"
            fi
            tail -n 60 "logs/tests-seed$seed.log"
          done
          exit $failed

      - name: Test [gpu] (macOS, MoltenVK)
        if: env.MACOS_GPU_LANE == 'true'
        env:
          SDL_VIDEODRIVER: cocoa
        run: |
          set -o pipefail
          scripts/run-tests.sh --gpu ${{ matrix.config }} --rng-seed "$SEED_A" 2>&1 | tee logs/tests-gpu.log

      # ------------------------------------------------------------------
      # Reports
      # ------------------------------------------------------------------
      # Both build scripts write logs/build-*.log.
      - name: Summarize build errors
        if: failure()
        run: |
          grep -hE "(error|undefined reference|Undefined symbols|ld: )" logs/build-*.log 2>/dev/null | sort -u | head -150 || true

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: test-results-${{ matrix.leg }}-${{ matrix.config }}
          path: test-results/
          if-no-files-found: ignore
          retention-days: 14

      - name: Upload logs
        if: failure()
        uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: logs-${{ matrix.leg }}-${{ matrix.config }}
          path: |
            logs/
            bin/*/ArcaneTests/diagnostics/
            bin/*/ArcaneTests/*.log
            bin/*/ArcaneTests/ReferenceProject/Saved/Verify/
          if-no-files-found: ignore
          retention-days: 14

  # Discord: post this repo's CI result into its thread in the ⚙️・ci-builds forum (shared action:
  # StarworksDev/.github/actions/discord-ci-notify; FAILED on every red run, RECOVERED on the first green
  # after one, failing jobs named). Never for pull requests (fork PRs get no secrets; roadmap D26).
  notify:
    name: Discord notify
    needs: [image-tag, ci-image, build-test-linux, build-test]
    if: always() && github.event_name != 'pull_request'
    runs-on: ubuntu-24.04
    permissions:
      actions: read
      contents: read
    steps:
      - uses: StarworksDev/.github/actions/discord-ci-notify@b2d36340a4b0bff117f0cfcba3564fe3fa984687 # main 2026-10-12
        with:
          result: ${{ contains(needs.*.result, 'failure') && 'failure' || (contains(needs.*.result, 'cancelled') && 'cancelled' || 'success') }}
          webhook: ${{ secrets.DISCORD_CI_WEBHOOK }}
          threads: ${{ vars.DISCORD_CI_THREADS }}
          github-token: ${{ github.token }}
```

- [ ] **Step 2: Lint the workflow**

```bash
docker run --rm -v "$(pwd -W):/repo" -w /repo rhysd/actionlint:1.7.7 -color .github/workflows/ci.yml
```

Expected: no output, exit 0. actionlint knows `needs` in `container.image` and `concurrency`. If the pinned tag is unavailable, use the newest `rhysd/actionlint` tag shown on Docker Hub; it is a dev-only check, not a pin in the repo.

- [ ] **Step 3: The static security review (spec s3.5), on the file**

```bash
grep -n "packages: write" .github/workflows/ci.yml                       # exactly one hit, inside ci-image
grep -nE "uses: [^ ]+@" .github/workflows/ci.yml | grep -vE "@[0-9a-f]{40}( |$)"   # no output
grep -n "secrets\." .github/workflows/ci.yml                             # only DISCORD_CI_WEBHOOK, in notify
grep -n "pull_request_target" .github/workflows/*.yml                    # no output
```

Expected: as commented. `notify` is the only job that reads a secret, and its `if:` excludes `pull_request`.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml
git commit -F - <<'EOF'
ci: ci.yml calls the shared scripts -- image-tag + ci-image jobs (content-addressed GHCR image, packages: write on ci-image only), Linux legs in the image by digest, Windows/macOS legs on scripts/ci/build.{ps1,sh}; the Discord action pinned to a SHA

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 10: Docs — `docs/ci.md`, `README.md`, `CLAUDE.md`

**Files:**
- Modify: `docs/ci.md`
- Modify: `README.md` (the `### CI` section)
- Modify: `CLAUDE.md` (the `## CI` section)

**Interfaces:**
- Consumes: the file names, job names and flags from Tasks 1–9, exactly as committed.

- [ ] **Step 1: `docs/ci.md`, the Overview**

Replace the `## Overview` section body (from "[`.github/workflows/ci.yml`]" through "...seeds.") with:

```markdown
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
```

- [ ] **Step 2: `docs/ci.md`, the Matrix and Toolchains rows**

In the Matrix table, change the two Linux rows' "Runner image" cell from `` `ubuntu-24.04` `` to `` `ubuntu-24.04` host; the leg runs in the `arcane-ci` image ([Image](#image)) ``.

In `## Toolchains`, replace the `**Linux:**` bullet with:

```markdown
- **Linux:** the legs run inside the repo's CI image ([Image](#image)):
  `g++-14` / `clang-19` from the Ubuntu 24.04 archive (one apt snapshot), the
  pinned SDL3, premake, DXC and Vulkan validation layer baked in. The leg
  exports `CC` / `CXX`; `scripts/ci/build.sh` gives premake `--cc=gcc|clang`
  from `CC`.
```

In the `**Images**` bullet, append: `` The Linux legs' container image is pinned by digest (see [Image](#image)). ``

- [ ] **Step 3: `docs/ci.md`, the Build driver**

Replace the whole `## Build driver` body with:

```markdown
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
| engine | `make -k ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient` | `msbuild Arcane.slnx` |
| game | `arcbuild build --project ReferenceProject --config <cfg> --sdk <checkout>` | the same |
| hosts | `make -k ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests` (their postbuild stages ReferenceProject, `Binaries/` included) | `msbuild Arcane.slnx` again, so every host restages the game module |

`build.sh` is bash 3.2-safe (macOS `/bin/bash`) and calls no git (desk
worktrees' `.git` file points at a Windows path a container cannot see).
`scripts/ci/tests/test-build-sh.sh` checks it against stub tools.
```

- [ ] **Step 4: `docs/ci.md`, the new Image and Local runs sections**

Insert both sections directly before `## Supply chain`:

````markdown
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
  `min(CPUs, memory GB / 2)` of what Docker reports,
  Windows `msbuild /m`; `-Jobs` sets both.
- **Output:** one table (leg, config, verdict, minutes, build, per-seed test
  counts, image, log directory), the first compiler/linker errors of a failed
  build, an OOM hint when the build logs show a compiler or linker killed by
  signal 9; logs under `logs\local\<leg>-<Config>\`.
  Exit 0 all passed, 1 a leg failed, 2 a preflight check failed (Docker not
  running, MSBuild missing -- each names its fix).
- macOS legs run on the hosted runner only.

`scripts\ci\compare-test-counts.ps1 -Baseline <dir> -Candidate <dir>`
compares two downloaded sets of `test-results-*` artifacts (or a local
`logs\local\...\test-results`) by `<leg>-<Config>`.
````

- [ ] **Step 5: `docs/ci.md`, Supply chain, Reproducing locally, Deviations**

In `## Supply chain`:
- Replace the first bullet's action list with `` (`actions/checkout` v7.0.1 with `persist-credentials: false`, `actions/cache` v6.1.0, `actions/upload-artifact` v7.0.2, `microsoft/setup-msbuild` v3, `docker/login-action` v4.6.0, `docker/setup-buildx-action` v4.4.1, and the org's `discord-ci-notify`). Cache keys are per image, never per configuration, for configuration-independent contents. ``
- Replace the second bullet with `` - The workflow token is read-only (`permissions: contents: read`); `ci-image` alone adds `packages: write`, and the Linux legs `packages: read`. ``
- Append the bullet `` - The Linux image's base is pinned by digest and its apt packages by one archive snapshot (`APT_SNAPSHOT` in `ci/linux/Dockerfile`). ``

In `## Reproducing locally`, insert as the first paragraph:

```markdown
On the desk, `scripts\ci\local.ps1` is the one command (see [Local runs](#local-runs)).
By hand, the sequences are the build scripts:
```

Then replace the Ubuntu block's make lines with `scripts/ci/build.sh Debug -j3` and keep its `xvfb-run ... run-tests.sh` line. The `CC=gcc-14 CXX=g++-14` prefix moves onto the `build.sh` line. In the PowerShell block, replace `GenerateProjects.bat` and `msbuild ...` with `scripts\ci\build.ps1 Debug`.

In `## Deviations`:
- Replace the row "SDL3 and the Vulkan validation layer are built from source on Linux/macOS" with:
  `| The Linux legs run in a repo-defined container image (`ci/linux/Dockerfile`) instead of on the bare runner image | One toolchain for hosted CI and the desk (`scripts/ci/local.ps1`); SDL3 and the validation layer (not packaged / too old on 24.04) are built once per image instead of per leg. Astra and Manifold2D may adopt the pattern. |`
- Add the row:
  `| macOS still builds SDL3 from source per runner (cached) | No macOS containers; the hosted macOS image is the environment. |`

- [ ] **Step 6: `README.md` and `CLAUDE.md`**

README `### CI`: after the first paragraph (ending "There is no ASan or TSan leg."), add:

```markdown
The Linux legs run inside the repo's CI image (`ci/linux/Dockerfile`). To run
the same build and tests on the desk before pushing: `scripts\ci\local.ps1`
(see [docs/ci.md](docs/ci.md#local-runs)).
```

CLAUDE.md `## CI`: append to the section's paragraph:

```markdown
Run the CI legs locally before pushing with `scripts\ci\local.ps1` (Windows
natively, Linux in the CI image; one-time `scripts\ci\setup-desk.ps1`). The
build steps live in `scripts/ci/build.{sh,ps1}`, not in the workflow YAML.
```

- [ ] **Step 7: Check every path the docs name exists**

```bash
for p in ci/linux/Dockerfile ci/linux/image-inputs.txt ci/linux/verify-image.sh scripts/ci/image-tag.sh scripts/ci/image-tag.ps1 \
         scripts/ci/build.sh scripts/ci/build.ps1 scripts/ci/ci-image.sh scripts/ci/local.ps1 scripts/ci/setup-desk.ps1 \
         scripts/ci/container-leg.sh scripts/ci/rsync-filter.txt scripts/ci/compare-test-counts.ps1 scripts/ci/tests/test-build-sh.sh; do
  [ -f "$p" ] || echo "MISSING $p"; done
```

Expected: no output.

- [ ] **Step 8: Commit**

```bash
git add docs/ci.md README.md CLAUDE.md
git commit -F - <<'EOF'
docs(ci): the Image and Local runs sections, Build driver = scripts/ci/build.{sh,ps1}, the container deviation; README/CLAUDE.md point at scripts\ci\local.ps1

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
EOF
```

---

### Task 11: Desk acceptance before any push

**Files:** none (evidence only; record results in the PR description draft at `logs/local/acceptance.md`, gitignored).

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: The whole local suite of script tests**

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci/tests/test-arcane-ci.ps1 | tail -n 1
scripts/ci/tests/test-image-tag.sh | tail -n 1
docker run --rm -v "$(pwd -W):/r:ro" arcane-ci:dev bash -c 'cp -r /r /tmp/r && cd /tmp/r && scripts/ci/tests/test-image-tag.sh && scripts/ci/tests/test-build-sh.sh && scripts/ci/tests/test-ci-image.sh' | grep -E "all passed|failure"
```

Expected: `all checks passed` and three `all passed` lines.

- [ ] **Step 2: Spec s3.3 (desk half): both default legs with CI's seed pair**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\local.ps1 -Seeds 2 -Seed 1; "exit=$LASTEXITCODE"
```

Expected:
- `PASS windows-msvc Debug` (`~[gpu]` desk baseline) and `PASS linux-gcc-14 Debug`, each with two seed entries;
- `exit=0`.

Save the summary into `logs/local/acceptance.md`.

- [ ] **Step 3: Spec s3.4 (desk half): red is visible**

```bash
git switch -c ci/red-probe
```

Append `static_assert(false, "red probe: the local CI summary must show this");` to `ArcaneCore/src/ArcaneCore.cpp`. Then, in PowerShell:

```powershell
powershell -File scripts\ci\local.ps1 -Leg linux-gcc-14; "exit=$LASTEXITCODE"
```

Expected:
- `FAIL linux-gcc-14 Debug  ... build FAILED (2)`, or make's exit code;
- a `first build errors` block containing `red probe`;
- `exit=1`.

Then discard the probe (it is never merged):

```bash
git checkout -- . && git switch ci/local-pipeline && git branch -D ci/red-probe
```

- [ ] **Step 4: The fuller local matrix (optional, time permitting)**

`powershell -File scripts\ci\local.ps1 -Full` (about 3–5 h). Expected: nine `PASS` rows. If it is skipped, say so in the PR description; the hosted run covers it.

---

### Task 12: Rollout — baseline, PR, publish, parity (needs the user's go-ahead)

**Files:** none in the repo, unless parity finds a defect.

**Interfaces:**
- Consumes: the branch; GitHub (`gh` at `/c/Program Files/GitHub CLI/gh.exe`).

- [ ] **Step 1: Capture the pre-container baseline (read-only)**

```bash
GH="/c/Program Files/GitHub CLI/gh.exe"
"$GH" run download 38154946856 -R StarworksDev/Arcane -p 'test-results-*' -D logs/parity/baseline
ls logs/parity/baseline | head -20
```

Run `38154946856` is `main` @ `946092fb`, green, 2026-10-11 — this branch's base. Expected: 12 `test-results-<leg>-<Config>` directories. The Linux gcc Debug directory includes the `-gpu` JUnit.

- [ ] **Step 2: ASK the user before pushing**

Show the user:
- `git log --oneline main..ci/local-pipeline`;
- the Task 11 summary;
- the planned PR title: `ci: local CI pipeline -- repo-defined Linux image, scripts/ci entry points, scripts\ci\local.ps1 (sub-project A)`.

Ask for the go-ahead to push the branch and open the PR. **Do not push without it.**

- [ ] **Step 3: Push and open the PR (after the go-ahead)**

```bash
git push -u origin ci/local-pipeline
"$GH" pr create -R StarworksDev/Arcane --base main --head ci/local-pipeline \
  --title "ci: local CI pipeline -- repo-defined Linux image, scripts/ci entry points, scripts\\ci\\local.ps1 (sub-project A)" \
  --body-file logs/local/acceptance.md
```

The body is `acceptance.md`, expanded with:
- a summary;
- the spec and plan paths;
- the resolutions table (R1–R14);
- the one manual step in Step 5 below;
- the test plan;
- the trailing attribution lines (🤖 Generated with [Claude Code](https://claude.com/claude-code), then the session URL).

- [ ] **Step 4: Watch the first run**

```bash
"$GH" run list -R StarworksDev/Arcane --branch ci/local-pipeline --limit 1
"$GH" run watch <run-id> -R StarworksDev/Arcane --exit-status
```

Expected:
- `CI scripts + image tag` green;
- `CI image` builds and pushes (~25–40 min) and prints `ci-image: ghcr.io/starworksdev/arcane-ci:build-<tag> = sha256:...`;
- the six Linux legs start in the container and pull with the workflow token;
- all 12 legs green.

A red leg is diagnosed from its logs artifact. Each fix is its own commit, pushed with the user's standing go-ahead for this PR; keep the user informed.

- [ ] **Step 5: Make the package public (the user, or an org admin)**

Ask the user to do this once, at github.com/orgs/StarworksDev/packages/container/arcane-ci/settings:
1. Danger Zone → Change visibility → Public.
2. Confirm "Manage Actions access" lists `StarworksDev/Arcane` (the Dockerfile's `org.opencontainers.image.source` label links it).

Then verify from the desk without credentials:

```powershell
docker logout ghcr.io; docker pull ghcr.io/starworksdev/arcane-ci:<tag>
```

Expected: the pull succeeds. Then run `powershell -File scripts\ci\local.ps1 -Leg linux-gcc-14`: its summary now shows `image: build-<tag>` with no `-local`.

- [ ] **Step 6: Spec s3.2, container parity**

```bash
"$GH" run download <run-id> -R StarworksDev/Arcane -p 'test-results-*' -D logs/parity/candidate
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci/compare-test-counts.ps1 -Baseline logs/parity/baseline -Candidate logs/parity/candidate
```

Expected:
- every key `Match = True`, `all <N> key(s) match`. This covers gcc and clang × 3 configs plus `linux-gcc-14-Debug-gpu`, and Windows/macOS unchanged.
- The `Windowed smoke (Linux, X11 + Wayland)` step is green, and its `logs-linux-gcc-14-Debug` artifact shows `smoke: ok` for runtime/editor × x11/wayland.

A mismatch means stop and diagnose before anything else; a container-only difference is a defect in this branch.

- [ ] **Step 7: Spec s3.3 (CI half): local equals CI for the same leg**

```powershell
powershell -File scripts\ci\local.ps1 -Leg linux-gcc-14 -Config Debug -Seeds 2 -Seed <run_number of the CI run>
powershell -File scripts\ci\compare-test-counts.ps1 -Baseline logs\parity\candidate\test-results-linux-gcc-14-Debug -Candidate logs\local\linux-gcc-14-Debug\test-results -Key linux-gcc-14-Debug
```

Expected: `linux-gcc-14-Debug` matches. `-Key` drops the CI artifact's `-gpu` JUnit, since the local leg runs no GPU lane. The local run is on the published image, so its label has no `-local`.

- [ ] **Step 8: Spec s3.4 (CI half): red is visible in CI (ask first)**

With the user's OK, push a throwaway `ci/red-probe` branch: Task 11 Step 3's probe, on top of `ci/local-pipeline`. Open it as a **draft** PR, which is a `pull_request` event, so there is no Discord post.

Confirm:
- the Linux and Windows legs go red;
- `Summarize build errors` shows `red probe`.

Then close the draft PR and run `git push origin --delete ci/red-probe`. Nothing is merged.

- [ ] **Step 9: Spec s3.5 on the live run**

In the run's `CI image` job log, confirm the `GITHUB_TOKEN` permissions block lists `Packages: write`. In a Linux leg's log, confirm `Packages: read` and `Contents: read`, and no `write`.

Re-run Task 9 Step 3's greps on the pushed `ci.yml`.

- [ ] **Step 10: Hand back**

Report to the user:
- the run URL;
- the parity table;
- the no-change sync time;
- the image tag and digest;
- whether `-Full` ran locally.

**Merging the PR is the user's call.** After the merge, the next step is sub-project B's brainstorm, per the spec s5 rollout.

---

## Audit amendments (2026-10-12)

The cross-repo Actions audit (`StarworksDev/.github` `docs/ci-audit-2026-10-12.md`) changed this plan in place:

- **Global Constraints and Task 9:** node24 action pins (C3) and `persist-credentials: false` (C7).
- **Task 9:** cache keys per image, not per config (C9).
- **Task 3:** `libclang-rt-19-dev` in the image, for F2 below.
- **Task 10:** the docs follow those changes.

It also adds Tasks 13–14 and the follow-ons F1–F3. Tasks 13–14 run after Task 12's PR is green. Each is its own commit on the same branch, pushed with the user's standing go-ahead for that PR.

### Task 13: Notify through `discord-ci-notify` v1 (audit C1, C2)

**Precondition:** the `StarworksDev/.github` branch `discord-ci-notify-v1` is merged, its `Test` workflow is green, and the tag `discord-ci-notify-v1.0.0` exists (the user's go-ahead).

**Files:**
- Modify: `.github/workflows/ci.yml` (the `notify` job)
- Modify: this plan's Global Constraints (the notify pin line)

- [ ] **Step 1: Resolve the tag's commit**

```bash
SHA=$(git ls-remote https://github.com/StarworksDev/.github 'refs/tags/discord-ci-notify-v1.0.0^{}' | cut -f1)
[ -n "$SHA" ] || SHA=$(git ls-remote https://github.com/StarworksDev/.github refs/tags/discord-ci-notify-v1.0.0 | cut -f1)
echo "$SHA" | grep -qE '^[0-9a-f]{40}$' && echo "v1.0.0 = $SHA"
```

Expected: `v1.0.0 = <40 hex>`. `^{}` peels an annotated tag; a lightweight tag falls through to the second line.

- [ ] **Step 2: Point the notify job at it, passing `needs`**

Write `scratch/notify-v1.py` with the Write tool, then run it with `python scratch/notify-v1.py "$SHA"`:

```python
import sys
sha = sys.argv[1]
P = '.github/workflows/ci.yml'
s = open(P, encoding='utf-8').read()
old_uses = "StarworksDev/.github/actions/discord-ci-notify@b2d36340a4b0bff117f0cfcba3564fe3fa984687 # main 2026-10-12"
old_result = ("          result: ${{ contains(needs.*.result, 'failure') && 'failure' || "
              "(contains(needs.*.result, 'cancelled') && 'cancelled' || 'success') }}\n")
old_head = "    if: always() && github.event_name != 'pull_request'\n    runs-on: ubuntu-24.04\n    permissions:\n      actions: read"
assert s.count(old_uses) == 1 and s.count(old_result) == 1 and s.count(old_head) == 1
s = s.replace(old_uses, f"StarworksDev/.github/actions/discord-ci-notify@{sha} # discord-ci-notify-v1.0.0")
s = s.replace(old_result, "          needs: ${{ toJSON(needs) }}\n")
s = s.replace(old_head, old_head.replace("    runs-on: ubuntu-24.04\n", "    runs-on: ubuntu-24.04\n    timeout-minutes: 5\n"))
open(P, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
```

Expected: `ok`. `grep -n "discord-ci-notify@\|toJSON(needs)\|timeout-minutes: 5" .github/workflows/ci.yml` shows the pinned `uses:`, the `needs:` input, and the notify job's timeout. Update the Global Constraints line that names the notify pin to the same SHA and tag.

- [ ] **Step 3: Lint and commit**

Run `docker run --rm -v "$(pwd -W):/repo" -w /repo rhysd/actionlint:1.7.7 -color .github/workflows/ci.yml` and expect no output. Then commit `.github/workflows/ci.yml` and this plan:

```
ci: Discord notify through discord-ci-notify v1 -- pinned to the release tag's commit, classifies the run from needs (a skipped job is INCOMPLETE, never success)
```

The commit carries the attribution lines.

### Task 14: Jenkins-only checks that need no GPU move to the hosted Windows legs (audit C10)

The Jenkinsfile's "Asset cook check" (`arccook --check`, Debug + Release) and "ReferenceProject (SDK build)" (premake `vs2026` + msbuild on `ReferenceProject.slnx`) need no GPU. Today they run only on the desk. They move into a script both CI and B's runner can call. Jenkins keeps running them until B retires it.

**Files:**
- Create: `scripts/ci/reference-checks.ps1`
- Modify: `.github/workflows/ci.yml` (one step in `build-test`)
- Modify: `docs/ci.md` (Extra lanes table)

- [ ] **Step 1: Find out whether `--check` passes on a fresh checkout (desk)**

The Jenkins agent's workspace persists, so its cook state may never have been cold. In a fresh worktree:

```bash
git worktree add ../Arcane-coldcheck HEAD && cd ../Arcane-coldcheck
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci/build.ps1 Debug
bin/Debug-windows-x86_64-md/arccook/arccook.exe --project ReferenceProject --check; echo "check exit=$?"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-tests.ps1 Debug --rng-seed 1 > /dev/null
bin/Debug-windows-x86_64-md/arccook/arccook.exe --project ReferenceProject --check; echo "after tests exit=$?"
cd .. && git worktree remove --force Arcane-coldcheck
```

Record both exit codes:
- `check exit=0`: the check could run right after the build. The script still runs after the tests, which keeps one order.
- Non-zero before the tests and 0 after: the after-tests order is required. Say so in the script header.
- Non-zero both times: **stop and report to the user.** The check then needs a cook step first (`arccook --project ReferenceProject` without `--check`), which is a decision about what "nothing stale" means on a clean machine.

- [ ] **Step 2: Write the script**

```powershell
# reference-checks.ps1 <Debug|Release|Dist> -- the Jenkins-gate checks that
# need no GPU, run on the hosted Windows legs after the tests
# (.github/workflows/ci.yml; audit C10). Sub-project B's runner calls the same
# script. Run AFTER scripts\ci\build.ps1 and scripts\run-tests.ps1:
#   * Debug and Release: arccook --project ReferenceProject --check -- the
#     cooked content is current (the Jenkinsfile's "Asset cook check").
#   * Debug: ReferenceProject built as its own workspace (premake vs2026 +
#     msbuild ReferenceProject.slnx) -- a game author's path without arcbuild
#     (the Jenkinsfile's "ReferenceProject (SDK build)").
# Exit: 0, or the first failing check's code.
param([Parameter(Mandatory, Position = 0)][ValidateSet('Debug', 'Release', 'Dist')][string]$Config)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logs = Join-Path $root 'logs'

if ($Config -ne 'Dist') {
    $arccook = Join-Path $root "bin\$Config-windows-x86_64-md\arccook\arccook.exe"
    $code = Invoke-NativeLogged -FilePath $arccook -ArgumentList @('--project', (Join-Path $root 'ReferenceProject'), '--check') `
        -LogPath (Join-Path $logs "check-cook-$Config.log")
    if ($code -ne 0) { Write-Host "reference-checks: arccook --check FAILED ($code)" -ForegroundColor Red; exit $code }
}
if ($Config -eq 'Debug') {
    # premake runs inside ReferenceProject/, as the Jenkinsfile does.
    Push-Location (Join-Path $root 'ReferenceProject')
    try {
        $code = Invoke-NativeLogged -FilePath (Join-Path $root 'ThirdParty\premake5\premake5.exe') -ArgumentList @('vs2026') `
            -LogPath (Join-Path $logs 'check-sdk-generate.log')
    } finally { Pop-Location }
    if ($code -ne 0) { Write-Host "reference-checks: ReferenceProject premake FAILED ($code)" -ForegroundColor Red; exit $code }
    $code = Invoke-NativeLogged -FilePath (Resolve-MSBuild) `
        -ArgumentList @((Join-Path $root 'ReferenceProject\ReferenceProject.slnx'), '/p:Configuration=Debug', '/m', '/nologo', '/v:minimal') `
        -LogPath (Join-Path $logs 'check-sdk-build.log')
    if ($code -ne 0) { Write-Host "reference-checks: ReferenceProject standalone build FAILED ($code)" -ForegroundColor Red; exit $code }
}
Write-Host "reference-checks: $Config ok"
exit 0
```

- [ ] **Step 3: Run it on the desk**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\reference-checks.ps1 Debug; "exit=$LASTEXITCODE"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci\reference-checks.ps1 Release; "exit=$LASTEXITCODE"
```

Expected: `reference-checks: Debug ok` and `Release ok`, both `exit=0`, and the logs under `logs\`.

- [ ] **Step 4: Call it from the Windows legs**

In `build-test`, directly after the `Test (Windows, two seeds)` step:

```yaml
      # Jenkins parity, no GPU needed (audit C10): arccook --check (Debug,
      # Release) and ReferenceProject built standalone (Debug).
      - name: Reference checks (Windows)
        if: runner.os == 'Windows' && !cancelled() && matrix.config != 'Dist'
        shell: pwsh
        run: ./scripts/ci/reference-checks.ps1 ${{ matrix.config }}
```

Add a row to `docs/ci.md`'s Extra lanes table:

```
| `arccook --check` + ReferenceProject standalone build (`scripts/ci/reference-checks.ps1`) | `windows-msvc` Debug (both), Release (cook check) | the cooked sample content is current; a game workspace builds without arcbuild (moved from the Jenkins gate) |
```

Lint with actionlint (no output expected), then commit:

```
ci: arccook --check and the standalone ReferenceProject build move from Jenkins to the hosted Windows legs (scripts/ci/reference-checks.ps1)
```

The commit carries the attribution lines.

### Follow-ons after A merges (planned separately; each needs A's measurements)

**F1. Hosted compiler cache (audit C8).** This is the largest remaining time cost.

| Leg | Approach |
|---|---|
| Linux (container) | `PATH=/usr/lib/ccache:$PATH`, `CCACHE_DIR` in the workspace, `actions/cache` keyed `ccache-<leg>-<Config>-<sha>` with a `ccache-<leg>-<Config>-` restore prefix. Saved on `main` pushes only, so PRs restore but never pollute. |
| macOS | ccache from its release tarball, SHA-256 pinned (not unpinned `brew`). |
| Windows | ccache's MSBuild mode or sccache: needs a premake option for `/Z7` object debug info (the linker still writes the PDB) plus `/p:UseMultiToolTask=true`. Prove it on the desk first. |

`max-size` per leg comes from Task 8's measured `ccache -s` on the desk. The total must stay under ~8 GB of the repo's 10 GB, now that C9 freed the duplicated tool caches.

Acceptance: a warm-run timing table against run 38154946856; a cold run no slower than +5%; identical test counts.

**F2. ASan + UBSan lane (audit P1).**
1. Add `--sanitize=address|thread` to Arcane's `premake5.lua`, with the same shape as Astra, Manifold2D and Mosaic.
2. Triage locally in the image: linux-clang-19 Debug `--sanitize=address`, on the `~[gpu]` CPU suite.
3. Fix or suppress findings, each with a reason. LSan suppressions live in `scripts/ci/lsan-suppressions.txt`, in the same `<pattern> | <reason>` discipline as the test exclusions.
4. Add the lane to `build-test-linux` only when it is green on the desk.
5. TSan for the job system is a later lane.

**F3. Org / repo settings (user or admin; audit C5, C6, C4).**
- Fork-PR approval → `all_external_contributors` on all five repos.
- `can_approve_pull_request_reviews` → false at the org.
- `sha_pinning_required` → true at the org, once every repo's pinning PR has merged.

## Final review amendments (2026-10-12)

The whole-branch review found two defects in this plan's own code; the branch carries the fixes, and any reuse (sub-project B) must take them, not Task 7's text:
- **Task 7 digest:** `grep '"digest":"sha256:...'` never matches buildx's pretty-printed `{{json .Manifest}}` (`"digest": "..."`), so `ci-image` would fail on every hosted run. `ci-image.sh` now hashes the raw manifest (`imagetools inspect --raw` into a file, then SHA-256), which equals the registry digest; the stub prints real-shaped output.
- **Task 7 order:** the image was pushed before `verify-image.sh` ran, so a failed verify left a published tag later runs reused unverified. Now `buildx build --load`, verify, then `docker push`; a failed verify publishes nothing.
- `Invoke-NativeLogged` reads a native stderr line's text from `TargetObject` (Windows PowerShell 5.1) or `Exception.Message` (pwsh 7).
- `test-image-tag.sh`'s "desk flake" was `MSYS_NO_PATHCONV=1` (this plan's desk shell rule) stopping Git Bash from translating the paths handed to `powershell.exe` and `git.exe`; both now go through `cygpath -w`, and the twins' stderr is kept for FAIL messages.
