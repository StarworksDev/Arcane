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
# CI's pair (N, (N + 50021) mod 4294967296); -Seed N fixes N (CI's N is the run number). Jobs:
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
        # A wedged Docker Desktop (API 500) still exits 0, printing ' 0 0' (an empty OSType).
        if ($info.Code -ne 0 -or -not $info.Output -or "$($info.Output[0])" -notmatch '^\S') {
            $problems += 'Docker is not running or not responding: start (or restart) Docker Desktop'
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

    # CI's Linux checkout has LF where this working tree (core.autocrlf) has
    # CRLF; each Linux leg converts these files after its mirror, so it builds
    # and tests CI's bytes (SweepCoverageTest read names ending in a carriage return).
    # git runs here on the desk, never in the container.
    $lfFiles = @()
    $prevEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
        $eol = Invoke-NativeQuiet -FilePath 'git' -ArgumentList @('-C', $root, '-c', 'core.quotepath=off', 'ls-files', '--eol')
        if ($eol.Code -eq 0) { $lfFiles = Get-LfCheckoutPaths -EolLines $eol.Output }
        else { throw "git ls-files exited $($eol.Code)" }
    } catch {
        Write-Host "local: WARNING cannot list CRLF files ($($_.Exception.Message)) -- Linux legs mirror them as-is and may differ from CI" -ForegroundColor Yellow
    } finally {
        [Console]::OutputEncoding = $prevEncoding
    }
    Write-Host "local: $($lfFiles.Count) CRLF file(s) become LF in each Linux leg (as CI's checkout has them)"
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
    [System.IO.File]::WriteAllText((Join-Path $out 'lf-files.txt'), (($lfFiles | ForEach-Object { "$_`n" }) -join ''),
        (New-Object System.Text.UTF8Encoding($false)))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    # Docker again, right before the leg: a Windows leg outlasts Docker Desktop's
    # Resource Saver timeout (5 min by default), and some versions come back from
    # that pause wedged (API 500; `docker info` exits 0 with an empty OSType).
    # Allow a minute for a resume, then name it instead of "container exit 125".
    $dockerUp = $false
    for ($i = 0; $i -lt 12; $i++) {
        $probe = Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('info', '--format', '{{.OSType}}')
        if ($probe.Code -eq 0 -and "$($probe.Output)" -match '^linux') { $dockerUp = $true; break }
        Start-Sleep -Seconds 5
    }
    if (-not $dockerUp) {
        Write-Host 'local: Docker Desktop is not responding -- restart it (Resource Saver can leave it wedged; docs/ci.md "Local runs")' -ForegroundColor Red
        $status = [ordered]@{ docker = 1 }
        Write-LegStatus -Path (Join-Path $out 'status.txt') -Status $status
        return New-LegResult -Run $Run -Status $status -OutDir $out -Seeds $seedList -Seconds $sw.Elapsed.TotalSeconds -Image $image.Label
    }
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
# foreach, not a pipeline: Format-LocalSummary returns ONE array object.
foreach ($line in (Format-LocalSummary -Results $results)) { Write-Host $line }
if (@($results | Where-Object { -not $_.Passed }).Count -gt 0) { exit 1 }
exit 0
