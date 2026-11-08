#!/bin/sh
# generate-engine-constants.sh [--check] -- POSIX twin of generate-engine-constants.ps1.
# Writes the two checked-in outputs from build/engine-constants.lua:
#   Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp
#   data/shaders/bindless-capacity.hlsli
# No flag: write them (an output whose content already matches is left
# untouched, so its mtime does not move). --check: regenerate into a temp
# directory and exit 1 if either checked-in file differs (CR stripped before
# comparing); nothing committed is written. Exit 2: the lua field is missing or
# not an integer in 1..4096. POSIX sh: macOS runs this as the shader prebuild.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LUA="$ROOT/build/engine-constants.lua"
HPP_REL=Source/Client/ArcaneRender/src/Arcane/Render/Generated/BindlessCapacity.gen.hpp
HLSL_REL=data/shaders/bindless-capacity.hlsli

CHECK=0
case "${1:-}" in
    '') ;;
    --check) CHECK=1 ;;
    *) echo "usage: generate-engine-constants.sh [--check]" >&2; exit 2 ;;
esac

[ -f "$LUA" ] || { echo "generate-engine-constants: missing $LUA" >&2; exit 2; }
VALUE=$(tr -d '\r' < "$LUA" | sed -n 's/^[[:space:]]*bindlessCapacity[[:space:]]*=[[:space:]]*\([0-9][0-9]*\)[[:space:]]*\(--.*\)\{0,1\}$/\1/p' | head -n 1)
if [ -z "$VALUE" ]; then
    echo 'generate-engine-constants: build/engine-constants.lua has no integer "bindlessCapacity = N" line' >&2
    exit 2
fi
if [ "${#VALUE}" -gt 4 ] || [ "$VALUE" -lt 1 ] || [ "$VALUE" -gt 4096 ]; then
    echo "generate-engine-constants: bindlessCapacity $VALUE is outside 1..4096" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
cat > "$TMP/hpp" <<EOT
#pragma once
#include <cstdint>
#include <Arcane/Core/Constant.hpp>
namespace Arcane {
    ARC_CONSTANT("shader/GPU contract: bindless material table capacity; source of truth is build/engine-constants.lua")
    inline constexpr std::uint32_t kBindlessCapacity = $VALUE;
}
EOT
cat > "$TMP/hlsl" <<EOT
#ifndef ARC_BINDLESS_CAPACITY_HLSLI
#define ARC_BINDLESS_CAPACITY_HLSLI
#define kMeshBindlessCapacity $VALUE
#endif
EOT

same() {   # same <fresh> <checked-in>: CR-folded equality; false when missing
    [ -f "$2" ] || return 1
    tr -d '\r' < "$2" | cmp -s "$1" -
}

drift=0
for pair in "hpp:$HPP_REL" "hlsl:$HLSL_REL"; do
    fresh="$TMP/${pair%%:*}"
    dest="$ROOT/${pair#*:}"
    if same "$fresh" "$dest"; then continue; fi
    if [ "$CHECK" = 1 ]; then
        echo "generate-engine-constants: DRIFT in ${pair#*:} -- run scripts/generate-engine-constants.sh and commit" >&2
        drift=1
    else
        mkdir -p "$(dirname "$dest")"
        cp "$fresh" "$dest"
        echo "generate-engine-constants: wrote ${pair#*:}"
    fi
done
if [ "$CHECK" = 1 ]; then
    [ "$drift" = 0 ] || exit 1
    echo "generate-engine-constants: outputs match build/engine-constants.lua (bindlessCapacity = $VALUE)"
fi
exit 0
