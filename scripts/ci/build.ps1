# build.ps1 <Debug|Release|Dist> [-Jobs N] -- THE Windows build sequence:
# .github/workflows/ci.yml and scripts\ci\local.ps1 run exactly this. The
# Linux/macOS twin is scripts/ci/build.sh (same steps, same log names).
#   1. generate  GenerateProjects.bat (premake vs2026)
#   2. engine    msbuild Arcane.slnx  (the whole solution)
#      abi       arcane-abi-digest --check (build/abi-digest.txt + kGamePluginABIDigest)
#   3. game      arcbuild build --project ReferenceProject --config <cfg> --sdk <checkout>
#   4. hosts     msbuild Arcane.slnx again, so every host's postbuild restages
#                the game module
# Stops at the first failing step and exits with its code. Logs:
# logs\build-{generate,engine,abi,referencegame,tests}.log (UTF-8); stale ones go
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
Step 'abi' 'build-abi.log' (Join-Path $root "Binaries\$Config-windows-x86_64-md\arcane-abi-digest\arcane-abi-digest.exe") `
    @('--check', '--root', $root)
Step 'game' 'build-referencegame.log' (Join-Path $root "Binaries\$Config-windows-x86_64-md\arcbuild\arcbuild.exe") `
    @('build', '--project', (Join-Path $root 'ReferenceProject'), '--config', $Config, '--sdk', $root)
Step 'hosts' 'build-tests.log' $msbuild @($solution, "/p:Configuration=$Config", $m, '/nologo', '/v:minimal')
Write-Host "build: $Config ok"
exit 0
