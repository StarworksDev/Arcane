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
# (ci.yml derives the same pair in bash). Catch2's --rng-seed is uint32, so
# a seed that does not itself fit uint32 is refused. The added seed always fits.
function Get-SeedList {
    param([Parameter(Mandatory)][string]$Seed, [ValidateSet(1, 2)][int]$Seeds = 1)
    if ($Seed -notmatch '^[0-9]+$') { throw "local: -Seed must be a non-negative integer (got '$Seed')" }
    if ([decimal]$Seed -gt 4294967295) { throw "local: seed $Seed does not fit Catch2's 32-bit --rng-seed" }
    $list = @($Seed)
    if ($Seeds -eq 2) {
        $sum = [uint64]$Seed + 50021
        while ($sum -gt 4294967295) { $sum -= 4294967296 }
        $list += "$sum"
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

# The files a Linux checkout of this tree holds as LF but this (Windows,
# core.autocrlf) working tree holds as CRLF: index LF, worktree CRLF, and no
# eol=crlf attribute (those stay CRLF everywhere). Input: the lines of
# `git -c core.quotepath=off ls-files --eol` ("i/.. w/.. attr/..<TAB>path").
# A local Linux leg converts these after its mirror, so it compiles and tests
# the bytes CI's Linux checkout has (container-leg.sh, lf-files.txt).
function Get-LfCheckoutPaths {
    param([AllowEmptyCollection()][string[]]$EolLines = @())
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($l in $EolLines) {
        if (-not $l) { continue }
        $tab = $l.IndexOf("`t")
        if ($tab -lt 0) { continue }
        $info = @($l.Substring(0, $tab).Trim() -split '\s+')
        if ($info.Count -lt 3) { continue }
        if ($info[0] -ne 'i/lf' -or $info[1] -ne 'w/crlf') { continue }
        if (($info[2..($info.Count - 1)] -join ' ') -match 'eol=crlf') { continue }
        $paths.Add($l.Substring($tab + 1))
    }
    return ,$paths.ToArray()
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
    } elseif ($Status.Contains('docker')) {
        $build = 'not run (Docker Desktop is not responding -- restart it; Resource Saver can wedge it, docs/ci.md "Local runs")'
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
            $passed = $false   # a seed that printed no summary proved nothing
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
            # A native stderr line arrives as an ErrorRecord, whose ToString() of an
            # EMPTY line is "...RemoteException". Its text: TargetObject on Windows
            # PowerShell 5.1, Exception.Message on pwsh 7 (TargetObject is null there).
            $line = "$_"
            if ($_ -is [System.Management.Automation.ErrorRecord]) {
                if ($_.TargetObject -is [string]) { $line = $_.TargetObject }
                elseif ($_.Exception) { $line = [string]$_.Exception.Message }
            }
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

# The DLL names under `dumpbin /dependents`' "Image has the following
# dependencies:" heading, up to the blank line that ends the list -- never
# the "Dump of file <path>" header, whose path may itself say Arcane.
function Get-DumpbinDependents {
    param([AllowEmptyString()][string[]]$Lines = @())
    $in = $false
    $seen = $false
    foreach ($line in $Lines) {
        if ($line -match 'has the following (delay load )?dependencies:') { $in = $true; continue }
        if (-not $in) { continue }
        $t = $line.Trim()
        if ($t -eq '') { if ($seen) { $in = $false; $seen = $false }; continue }
        $seen = $true
        $t
    }
}

Export-ModuleMember -Function Resolve-LocalRuns, Get-SeedList, Get-LinuxJobCount, ConvertTo-Bytes, Get-LfCheckoutPaths,
    Set-WslConfigLimits, Test-WslConfigLimits, Get-Catch2Totals, Get-JUnitTotals, Compare-JUnitDirs,
    Get-BuildErrorLines, Read-LegStatus, Write-LegStatus, New-LegResult, Format-LocalSummary,
    Invoke-NativeLogged, Invoke-NativeQuiet, Resolve-MSBuild, Get-DumpbinDependents
