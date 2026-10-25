# generate-engine-constants.ps1 [-Check]
# Writes the two checked-in outputs from build/engine-constants.lua:
#   Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp
#   data/shaders/bindless-capacity.hlsli
# No flag: write them (an output whose content already matches is left untouched,
# so its mtime does not move -- the shader prebuild runs this on every build).
# -Check: regenerate into a temp directory and exit 1 if either checked-in file
# differs (CRLF folded to LF before comparing; .gitattributes does not pin
# *.hlsli). Nothing committed is written in this mode. Exit 2: the lua field is
# missing or not an integer in 1..4096. Not the ABI digest tool; no premake, no DXC.
# Twin: scripts/generate-engine-constants.sh (writes the same bytes).
param([switch]$Check)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$luaPath = Join-Path $root 'build\engine-constants.lua'
$hppRel = 'Source\Client\ArcaneRender\src\Arcane\Render\Generated\BindlessCapacity.gen.hpp'
$hlslRel = 'data\shaders\bindless-capacity.hlsli'

function Fail([string]$Message, [int]$Code) {
    [Console]::Error.WriteLine("generate-engine-constants: $Message")
    exit $Code
}

if (-not (Test-Path -LiteralPath $luaPath)) { Fail "missing $luaPath" 2 }
$lua = [System.IO.File]::ReadAllText($luaPath)
$m = [regex]::Match($lua, '(?m)^[ \t]*bindlessCapacity[ \t]*=[ \t]*([0-9]+)[ \t]*(--.*)?\r?$')
if (-not $m.Success) { Fail 'build/engine-constants.lua has no integer "bindlessCapacity = N" line' 2 }
$value = [int]$m.Groups[1].Value
if ($value -lt 1 -or $value -gt 4096) { Fail "bindlessCapacity $value is outside 1..4096" 2 }

$hpp = @(
    '#pragma once'
    '#include <cstdint>'
    '#include <Arcane/Core/Constant.hpp>'
    'namespace Arcane {'
    '    ARC_CONSTANT("shader/GPU contract: bindless material table capacity; source of truth is build/engine-constants.lua")'
    "    inline constexpr std::uint32_t kBindlessCapacity = $value;"
    '}'
) -join "`n"
$hpp += "`n"
$hlsl = @(
    '#ifndef ARC_BINDLESS_CAPACITY_HLSLI'
    '#define ARC_BINDLESS_CAPACITY_HLSLI'
    "#define kMeshBindlessCapacity $value"
    '#endif'
) -join "`n"
$hlsl += "`n"

$outputs = @(
    @{ Rel = $hppRel; Text = $hpp },
    @{ Rel = $hlslRel; Text = $hlsl }
)
$utf8 = New-Object System.Text.UTF8Encoding $false

function Read-Normalized([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return ([System.IO.File]::ReadAllText($Path)).Replace("`r`n", "`n")
}

if ($Check) {
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("engine-constants-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $drift = $false
    try {
        foreach ($o in $outputs) {
            $fresh = Join-Path $tmp ([System.IO.Path]::GetFileName($o.Rel))
            [System.IO.File]::WriteAllText($fresh, $o.Text, $utf8)
            $a = Read-Normalized $fresh
            $b = Read-Normalized (Join-Path $root $o.Rel)
            if ($a -ne $b) {
                [Console]::Error.WriteLine("generate-engine-constants: DRIFT in $($o.Rel) -- run scripts\generate-engine-constants.ps1 and commit")
                $drift = $true
            }
        }
    }
    finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    if ($drift) { exit 1 }
    Write-Host "generate-engine-constants: outputs match build/engine-constants.lua (bindlessCapacity = $value)"
    exit 0
}

foreach ($o in $outputs) {
    $path = Join-Path $root $o.Rel
    if ((Read-Normalized $path) -eq $o.Text) { continue }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllText($path, $o.Text, $utf8)
    Write-Host "generate-engine-constants: wrote $($o.Rel)"
}
exit 0
