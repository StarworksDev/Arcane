# package.ps1 <Debug|Release|Dist> -- the monolithic ReferenceProject package
# (engine modularization s3.3 / s6) on Windows, staged with `arcbuild package`
# and run IN PLACE from Packages/ with no SDK, no Binaries/ and a bare PATH.
# scripts/ci/package.sh is the Linux/macOS twin. Run after
# scripts\ci\build.ps1 <Config>. The hosted runner has no GPU and the engine
# has no WARP selector, so this proves the package LOADS in place (every
# runtime file resolves from Packages/, --print-engine-info) and imports no
# Arcane DLL; the windowed boot is the desk's. It also checks HOST ISOLATION
# (ArcaneServer imports no Client/Developer/Editor module; the list comes from
# each .arcmodule's "type", with a positive control on ArcaneEngine.dll).
# Registrar survival (--print-registry) is NOT checked here: it is LINUX-ONLY
# for now (package.sh), so MSVC's /WHOLEARCHIVE + /OPT:REF has no witness in CI
# yet -- tracked in a follow-up issue. Logs: logs\package-*.txt.
param([Parameter(Mandatory, Position = 0)][ValidateSet('Debug', 'Release', 'Dist')][string]$Config)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null

& (Join-Path $root "Binaries\$Config-windows-x86_64-md\arcbuild\arcbuild.exe") package --project (Join-Path $root 'ReferenceProject') --config $Config --sdk $root
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$pkg = Join-Path $root "ReferenceProject\Packages\ReferenceProject-windows-$Config"
Get-ChildItem -Recurse $pkg -Depth 1 | Select-Object -ExpandProperty FullName

# Unreal's layout: the game in ReferenceProject\Binaries\Win64\, a stub
# launcher (same name) at the package root -- the boot goes through it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$dumpbin = @(& $vswhere -latest -find 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') | Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin.exe not found via vswhere' }
# Host isolation (modularization s6): the server imports no Client, Developer
# or Editor module. The forbidden list is each .arcmodule's "type" across
# Source\ AND ThirdParty\ (ImGui, SDL3, ImGuiNodeEditor live in the latter).
$server = Join-Path $root "Binaries\$Config-windows-x86_64-md\ArcaneServer\ArcaneServer.exe"
$serverDeps = @(Get-DumpbinDependents -Lines @(& $dumpbin /nologo /dependents $server))
$forbidden = @(Get-ChildItem -Recurse -File -Filter '*.arcmodule' (Join-Path $root 'Source'), (Join-Path $root 'ThirdParty') |
    ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json } |
    Where-Object { $_.type -in 'Client', 'Developer', 'Editor' } |
    ForEach-Object { "$($_.name).dll" })
if ($forbidden.Count -eq 0) { Write-Host '::error::host isolation: no Client/Developer/Editor module found under Source\ and ThirdParty\'; exit 1 }
if ($serverDeps.Count -eq 0) { Write-Host '::error::no dependency list for ArcaneServer.exe'; exit 1 }
# Positive control: a known-good import must match the naming scheme.
if (-not ($serverDeps -contains 'ArcaneEngine.dll')) { Write-Host "::error::host isolation: ArcaneServer's dependency list does not show ArcaneEngine.dll (naming scheme or dump broken): $($serverDeps -join ', ')"; exit 1 }
$leaks = @($serverDeps | Where-Object { $forbidden -contains $_ })
if ($leaks.Count -gt 0) { Write-Host "::error::ArcaneServer imports $($leaks -join ', ') (Client/Developer/Editor modules)"; exit 1 }
Write-Host "host isolation: ArcaneServer imports no Client/Developer/Editor module ($($forbidden.Count) checked: $($forbidden -join ', '))"

$dump = @(& $dumpbin /nologo /dependents (Join-Path $pkg 'ReferenceProject\Binaries\Win64\ReferenceProject.exe'))
$dump | Set-Content -Encoding utf8 (Join-Path $logs 'package-imports.txt')
$dump
# Only the dependency list: the dump's header names the image's path, and a
# checkout path (D:\a\Arcane\Arcane\ on a hosted runner) says Arcane.
$deps = @(Get-DumpbinDependents -Lines $dump)
if ($deps.Count -eq 0) { Write-Host '::error::no dependency list in the dumpbin output'; exit 1 }
if ($deps -match '^Arcane') { Write-Host "::error::the package imports an Arcane module DLL: $(($deps -match '^Arcane') -join ', ')"; exit 1 }

$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
Remove-Item Env:ARCANE_SDK -ErrorAction SilentlyContinue
# A Release package (and its launcher) is a WindowedApp: `&` would not wait
# for it, and $LASTEXITCODE would not be its exit code.
$out = Join-Path $logs 'package-info.txt'
$err = Join-Path $logs 'package-info-err.txt'
$p = Start-Process -FilePath (Join-Path $pkg 'ReferenceProject.exe') -ArgumentList '--print-engine-info' `
       -WorkingDirectory $pkg -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
Get-Content $out, $err
exit $p.ExitCode
