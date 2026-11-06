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
    Check 'seeds: the CI pair' ((((Get-SeedList -Seed '123' -Seeds 2) -join ',')) -eq '123,50144')
    Check 'seeds: 42949 pair stays in uint32' ((((Get-SeedList -Seed '42949' -Seeds 2) -join ',')) -eq '42949,92970')
    Check 'seeds: non-numeric refused' (Throws { Get-SeedList -Seed 'abc' })
    Check 'seeds: wrap stays in uint32' ((((Get-SeedList -Seed '4294967295' -Seeds 2) -join ',')) -eq '4294967295,50020')
    Check 'seeds: a seed past uint32 is refused' (Throws { Get-SeedList -Seed '4294967296' })

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

    # --- Get-LfCheckoutPaths (git ls-files --eol -> what a Linux checkout has as LF) ---
    $eolLines = @(
        "i/lf    w/crlf  attr/text=auto        `tscripts/settings-frozen-names.txt",
        "i/lf    w/lf    attr/text eol=lf      `tscripts/ci/build.sh",
        "i/lf    w/crlf  attr/text eol=crlf    `tscripts/run-tests.ps1",
        "i/lf    w/crlf  attr/text=auto eol=crlf `tci/msbuild.cmd",
        "i/crlf  w/crlf  attr/text=auto        `tlegacy/committed-crlf.txt",
        "i/-text w/-text attr/text=auto        `tdata/images/logo.png",
        "i/lf    w/crlf  attr/                 `tdocs/with space.md",
        '')
    $lf = Get-LfCheckoutPaths -EolLines $eolLines
    Check 'lf paths: CRLF on disk, LF in the index, no eol=crlf' (($lf -join '|') -eq 'scripts/settings-frozen-names.txt|docs/with space.md')
    Check 'lf paths: none' ((Get-LfCheckoutPaths -EolLines @()).Count -eq 0)

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
    Put (Join-Path $leg 'tests-seed8.log') "build output only, no catch2 summary`n"
    $nosum = New-LegResult -Run $run -Status ([ordered]@{ build = 0; 'tests-8' = 0 }) -OutDir $leg -Seeds @('8') -Image 'build-x'
    Check 'result: a seed with no Catch2 summary fails the leg' (-not $nosum.Passed)
    $ce = New-LegResult -Run $run -Status ([ordered]@{ container = 125 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: container never built' ((-not $ce.Passed) -and $ce.Build -like 'not run (container exit 125*')
    $dk = New-LegResult -Run $run -Status ([ordered]@{ docker = 1 }) -OutDir $leg -Seeds @('7') -Image 'build-x'
    Check 'result: Docker not responding is named, not a container exit' ((-not $dk.Passed) -and $dk.Build -like 'not run (Docker Desktop is not responding*')
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
    $log2 = Join-Path $tmp 'sub/native-empty.log'
    $null = Invoke-NativeLogged -FilePath $ps -ArgumentList @('-NoProfile', '-Command', "[Console]::Error.WriteLine('e1'); [Console]::Error.WriteLine(''); exit 0") -LogPath $log2
    Check 'native: an empty stderr line logs empty (5.1 RemoteException)' (-not ([System.IO.File]::ReadAllText($log2) -match 'RemoteException'))
    $q = Invoke-NativeQuiet -FilePath $ps -ArgumentList @('-NoProfile', '-Command', "Write-Output 'q'; exit 4")
    Check 'quiet: exit code and stdout' ($q.Code -eq 4 -and ($q.Output -join '') -eq 'q')

    # --- Get-DumpbinDependents ------------------------------------------------
    # dumpbin /dependents names the image in a header; on a hosted runner that
    # path is D:\a\Arcane\Arcane\..., so a plain -match 'Arcane' over the whole
    # output flags every package.
    $dump = @(
        'Dump of file D:\a\Arcane\Arcane\ReferenceProject\Packages\ReferenceProject-windows-Release\ReferenceProject\Binaries\Win64\ReferenceProject.exe',
        '', 'File Type: EXECUTABLE IMAGE', '', '  Image has the following dependencies:', '',
        '    bcrypt.dll', '    SDL3.dll', '    KERNEL32.dll', '    api-ms-win-crt-heap-l1-1-0.dll', '',
        '  Summary', '', '      1EF000 .data', '      535000 .text')
    $dll = @(Get-DumpbinDependents -Lines $dump)
    Check 'dependents: only the dependency list' (($dll -join ',') -eq 'bcrypt.dll,SDL3.dll,KERNEL32.dll,api-ms-win-crt-heap-l1-1-0.dll') ($dll -join ',')
    Check 'dependents: the header path is not a dependency' (-not ($dll -match 'Arcane'))
    $withArcane = $dump[0..7] + @('    ArcaneCore.dll') + $dump[8..($dump.Count - 1)]
    Check 'dependents: an Arcane module import is listed' (@(Get-DumpbinDependents -Lines $withArcane) -contains 'ArcaneCore.dll')
    Check 'dependents: no list, nothing' (@(Get-DumpbinDependents -Lines @('Dump of file x.exe', 'File Type: EXECUTABLE IMAGE')).Count -eq 0)
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($script:Failed -gt 0) { Write-Host "$($script:Failed) check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host 'all checks passed'
exit 0
