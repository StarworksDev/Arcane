# Compares a Catch2 JSON report against the committed baselines.
#
# WHY: the suite's counts were tracked by hand in session notes, which is not a
# mechanism -- nothing failed when they silently dropped. A count that goes DOWN
# means coverage was lost; going UP is normal and rewrites nothing on its own
# (the baseline is committed deliberately, so a rise is a reviewed edit).
#
# `baseline` is OPTIONAL-ABSENT rather than zero-defaulted. UE's own TelemetryData
# defaults it to 0, which makes "no baseline" and "baseline of zero"
# indistinguishable -- the same defaults-are-not-measurements trap this repo has
# been bitten by before.
#
# -Invocation IS MANDATORY, AND IT IS PART OF THE KEY. A count is only
# comparable against a count taken THE SAME WAY. The six committed baselines
# were measured under `ArcaneTests.exe "~[gpu]"`; the Jenkins lane runs the
# suite UNFILTERED on an agent where [gpu] contributes ~62000 further
# assertions. Feeding one to the other left a permanent +62000 of slack, so a
# regression that deleted a thousand non-GPU assertions would still have
# reported a rise and exited 0 -- the guard was live and effectively inert on
# the lane that matters most. Making the invocation part of the match means a
# mismatched lane has NO BASELINE and FAILS (exit 1) instead of passing on
# slack it never earned, unless scripts/baseline-exemptions.json names that
# (configuration, invocation) pair with a reason. Fail-closed beats
# not-checked-loudly beats cannot-fail-silently.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReportPath,
    [Parameter(Mandatory)][ValidateSet('Debug','Release','Dist')][string]$Configuration,
    # The test-spec the report was produced by, verbatim: "~[gpu]" for a
    # filtered run, "unfiltered" for a bare `ArcaneTests.exe`. Free text rather
    # than a ValidateSet, because it names whatever spec a lane actually passes
    # and a new one must be able to report "not checked" rather than be rejected.
    [Parameter(Mandatory)][string]$Invocation,
    [string]$BaselinePath
)

$ErrorActionPreference = 'Stop'

# Resolved HERE, not as a param() default: $PSScriptRoot is empty at the time
# param defaults are evaluated under Windows PowerShell 5.1, so the brief's
# Join-Path default threw "Cannot bind argument to parameter
# 'Path' because it is an empty string" on every invocation. In the body it is
# populated.
if (-not $BaselinePath) {
    $BaselinePath = Join-Path $PSScriptRoot 'automation-baselines.json'
}

if (-not (Test-Path $ReportPath)) {
    Write-Host "no Catch2 report at $ReportPath" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $BaselinePath)) {
    Write-Host "no baseline file at $BaselinePath" -ForegroundColor Red
    exit 1
}

$report   = Get-Content $ReportPath -Raw | ConvertFrom-Json
$baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json

# Catch2 3.15.0's JSON reporter shape, confirmed by RUNNING it, not by reading
# the writer:
#   { version, metadata{...}, test-run: { test-cases: [...],
#     totals: { assertions: {passed,failed,fail-but-ok,skipped},
#               test-cases: {passed,failed,fail-but-ok,skipped} } } }
# The totals live under `test-run`, NOT at the root, and `test-cases` needs
# quoting in PowerShell because of the hyphen. An earlier draft of this checker
# read `$report.totals`, which is $null -- and $null coerces to 0, so the
# vacuity guard below would itself have been vacuous.
$totals = $report.'test-run'.totals
if (-not $totals) {
    # NB: no embedded double quotes here. PowerShell does not use backslash as
    # a string escape (it uses backtick), so an inner " terminates the string
    # and the following [..] parses as an array index -- which is exactly how
    # this line was first written, and it was a parse error, not a runtime one.
    Write-Host "the report at $ReportPath has no 'test-run.totals'. Catch2 JSON shape has moved -- re-run ArcaneTests.exe with a non-matching tag plus --allow-running-no-tests -r json, and update this checker." -ForegroundColor Red
    exit 1
}

# Round-1 review finding: checking only that $totals exists is coarse. A
# renamed/dropped LEAF field (e.g. assertions.failed -> assertions.FOO) is
# $null under PowerShell's property access, and [int]$null is 0 -- silently
# summed into $assertions/$cases below. If the surviving leaf alone happened
# to equal the baseline, that read as a clean green pass with the vacuity
# guard never engaging (reproduced: renaming assertions.failed while passed
# stayed at the full baseline reported "+0" and exit 0). Validate each of the
# four leaf fields individually, by name, before summing anything.
$requiredLeaves = @(
    @{ Path = 'assertions.passed';       Value = $totals.assertions.'passed' }
    @{ Path = 'assertions.failed';       Value = $totals.assertions.'failed' }
    @{ Path = "'test-cases'.passed";     Value = $totals.'test-cases'.'passed' }
    @{ Path = "'test-cases'.failed";     Value = $totals.'test-cases'.'failed' }
)
$missingLeaves = $requiredLeaves | Where-Object { $null -eq $_.Value } | ForEach-Object { $_.Path }
if ($missingLeaves) {
    Write-Host "the report at $ReportPath is missing totals leaf field(s): $($missingLeaves -join ', '). Catch2 JSON shape has moved -- re-run ArcaneTests.exe with a non-matching tag plus --allow-running-no-tests -r json, and update this checker." -ForegroundColor Red
    exit 1
}

$assertions = [int]$totals.assertions.passed + [int]$totals.assertions.failed
$cases      = [int]$totals.'test-cases'.passed + [int]$totals.'test-cases'.failed

# THE VACUITY GUARD, the same property UE's XML parser enforces with
# `successes > 0`: a run that asserted nothing is not a pass, whatever its exit
# code said.
if ($assertions -le 0 -or $cases -le 0) {
    Write-Host "$Configuration ran $cases case(s) / $assertions assertion(s). A run that verified nothing is not a pass." -ForegroundColor Red
    exit 1
}

$exemptions = @()
$exemptionPath = Join-Path (Split-Path -Parent $BaselinePath) 'baseline-exemptions.json'
if (Test-Path $exemptionPath) {
    $exemptions = @((Get-Content $exemptionPath -Raw | ConvertFrom-Json).exemptions)
}

$failed = $false
foreach ($metric in @(
    @{ Name = 'arcanetests.assertions'; Actual = $assertions; Unit = 'assertions' },
    @{ Name = 'arcanetests.cases';      Actual = $cases;      Unit = 'cases' }
)) {
    # NAME *AND* CONFIGURATION *AND* INVOCATION. An entry with no `invocation`
    # field never matches: [string]$null is '' and no lane passes an empty
    # -Invocation, so an un-migrated baseline file fails (no baseline) rather
    # than silently comparing counts taken a different way.
    $entry = $baseline.baselines | Where-Object {
        $_.name -eq $metric.Name -and
        $_.configuration -eq $Configuration -and
        [string]$_.invocation -eq $Invocation
    }
    if (-not $entry) {
        # FAIL CLOSED. A lane with no measured baseline proves nothing, so it exits 1
        # unless scripts/baseline-exemptions.json names this (configuration, invocation)
        # pair with a non-empty reason.
        $exempt = $exemptions | Where-Object {
            $_.configuration -eq $Configuration -and [string]$_.invocation -eq $Invocation -and
            -not [string]::IsNullOrWhiteSpace([string]$_.reason)
        } | Select-Object -First 1
        if ($exempt) {
            Write-Host "telemetry: $($metric.Name) [$Configuration/$Invocation] = $($metric.Actual) $($metric.Unit) (EXEMPT -- not checked: $($exempt.reason))" -ForegroundColor Yellow
            continue
        }
        Write-Host "telemetry: $($metric.Name) [$Configuration/$Invocation] = $($metric.Actual) $($metric.Unit) -- NO BASELINE and no entry in scripts/baseline-exemptions.json. Measure it and commit a row, or exempt it with a reason." -ForegroundColor Red
        $failed = $true
        continue
    }
    $delta = $metric.Actual - [int]$entry.value
    if ($delta -lt 0) {
        Write-Host "telemetry: $($metric.Name) [$Configuration/$Invocation] = $($metric.Actual) $($metric.Unit), baseline $($entry.value) -- REGRESSED by $([Math]::Abs($delta)). Coverage was lost, or the baseline needs a reviewed update." -ForegroundColor Red
        $failed = $true
    } else {
        Write-Host "telemetry: $($metric.Name) [$Configuration/$Invocation] = $($metric.Actual) $($metric.Unit), baseline $($entry.value) (+$delta)" -ForegroundColor Green
    }
}

if ($failed) { exit 1 }
exit 0
