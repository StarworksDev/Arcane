# The ONE ArcaneTests entry point on Windows -- CI (.github/workflows/ci.yml)
# and a developer run exactly this. Linux/macOS twin: scripts/run-tests.sh
# (same arguments).
#
#   powershell -File scripts\run-tests.ps1 <Config> [--rng-seed N] [extra Catch2 args...]
#     Config: Debug | Release | Dist   (bin\<Config>-windows-x86_64-md\ArcaneTests)
#     --rng-seed N: the suite runs in RANDOM order; CI runs every leg with two
#             seeds. Omitted -> Catch2 picks one and prints it in the banner.
#
# Runs "~[gpu]" FROM the exe directory (fixtures, data\ and the staged
# automation-exclusions.json resolve relative to it), with reporters: console,
# JUnit at test-results\<leg>-<Config>-seed<N>.junit.xml (<leg> =
# $env:ARCANE_CI_LEG, default "windows") and a Catch2 JSON report beside it,
# which scripts\check-baselines.ps1 then compares against the committed counts.
# Under GitHub Actions the baseline key is "~[gpu] hosted-ci" (GitHub sets CI,
# so CrashPathTest skips its 7 reporter-spawn cases by design -- spec s6);
# elsewhere "~[gpu]". Exit status: the suite's, else the baseline check's.
#
# Plain $args parsing rather than param(): the GNU-style "--rng-seed N" must be
# accepted verbatim, exactly as run-tests.sh takes it.
$ErrorActionPreference = 'Stop'

if ($args.Count -lt 1) {
    Write-Host 'usage: run-tests.ps1 <Debug|Release|Dist> [--rng-seed N] [Catch2 args...]' -ForegroundColor Red
    exit 2
}
$Config = [string]$args[0]
if ($Config -notin @('Debug', 'Release', 'Dist')) {
    Write-Host "run-tests: config must be Debug, Release or Dist (got '$Config')" -ForegroundColor Red
    exit 2
}

$seed = $null
$extra = New-Object System.Collections.Generic.List[string]
for ($i = 1; $i -lt $args.Count; $i++) {
    $a = [string]$args[$i]
    if ($a -eq '--rng-seed') {
        if ($i + 1 -ge $args.Count) { Write-Host 'run-tests: --rng-seed needs a value' -ForegroundColor Red; exit 2 }
        $seed = [string]$args[$i + 1]
        $extra.Add('--rng-seed'); $extra.Add($seed)
        $i++
    } elseif ($a.StartsWith('--rng-seed=')) {
        $seed = $a.Substring(11)
        $extra.Add('--rng-seed'); $extra.Add($seed)
    } else {
        $extra.Add($a)
    }
}

$root    = Split-Path -Parent $PSScriptRoot
$exeDir  = Join-Path $root "bin\$Config-windows-x86_64-md\ArcaneTests"
$exe     = Join-Path $exeDir 'ArcaneTests.exe'
if (-not (Test-Path $exe)) {
    Write-Host "run-tests: $exe not built" -ForegroundColor Red
    exit 2
}

$leg      = if ($env:ARCANE_CI_LEG) { $env:ARCANE_CI_LEG } else { 'windows' }
$seedTag  = if ($seed) { $seed } else { 'random' }
$results  = Join-Path $root 'test-results'
New-Item -ItemType Directory -Force -Path $results | Out-Null
$junit    = Join-Path $results "$leg-$Config-seed$seedTag.junit.xml"
$json     = Join-Path $results "$leg-$Config-seed$seedTag.json"

Write-Host "run-tests: windows $Config, seed $seedTag, ~[gpu] -> $junit"
Push-Location $exeDir
try {
    & $exe '~[gpu]' @extra -r console -r "junit::out=$junit" -r "json::out=$json"
    $suiteExit = $LASTEXITCODE
} finally {
    Pop-Location
}

$invocation = if ($env:GITHUB_ACTIONS -eq 'true') { '~[gpu] hosted-ci' } else { '~[gpu]' }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check-baselines.ps1') `
    -ReportPath $json -Configuration $Config -Invocation $invocation
$baselineExit = $LASTEXITCODE

if ($suiteExit -ne 0) { exit $suiteExit }
exit $baselineExit
