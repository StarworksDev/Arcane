#!/bin/sh
# POSIX twin of compile-shaders.bat (Linux port, 2026-10-05): compiles every
# engine shader entry point to DXIL + SPIR-V loose artifacts under generated/.
# Invoked by the ArcaneClient prebuild step on non-Windows targets; also
# runnable by hand for the ARCANE_SHADER_DIR dev loop.
#
# SINGLE SOURCE OF TRUTH: this script does not carry its own entry-point list
# or flags. It reads compile-shaders.bat's `call :compile <src> <entry>
# <profile> <out>` lines and its `set SPIRV_FLAGS=` line, so the two scripts
# cannot drift -- add a shader to the .bat and Linux compiles it too.
#
# dxc resolution, first hit wins:
#   1. $ARCANE_DXC                                  (explicit path to a dxc binary)
#   2. ThirdParty/tools/dxc-linux/bin/dxc           (scripts/fetch-dxc-linux.sh)
#   3. ThirdParty/tools/dxc-macos/bin/dxc           (scripts/fetch-vulkan-sdk-macos.sh)
#   4. dxc on PATH
#
# macOS emits SPIR-V ONLY: a Mac runs the Vulkan backend (MoltenVK) and never
# loads DXIL, and the Vulkan SDK's DXC has no libdxil to sign DXIL with.
# ARCANE_SHADER_TARGETS ("dxil spirv" / "spirv") overrides the per-OS default.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SRC/../.." && pwd)
OUT="$SRC/generated"
BAT="$SRC/compile-shaders.bat"

DXC="${ARCANE_DXC:-}"
if [ -z "$DXC" ] && [ -x "$ROOT/ThirdParty/tools/dxc-linux/bin/dxc" ]; then
    DXC="$ROOT/ThirdParty/tools/dxc-linux/bin/dxc"
fi
if [ -z "$DXC" ] && [ -x "$ROOT/ThirdParty/tools/dxc-macos/bin/dxc" ]; then
    DXC="$ROOT/ThirdParty/tools/dxc-macos/bin/dxc"
fi
if [ -z "$DXC" ] && command -v dxc >/dev/null 2>&1; then
    DXC=$(command -v dxc)
fi
if [ -z "$DXC" ]; then
    echo "compile-shaders.sh: no dxc found. Run scripts/fetch-dxc-linux.sh (Linux) or scripts/fetch-vulkan-sdk-macos.sh (macOS), put dxc on PATH, or set ARCANE_DXC." >&2
    exit 1
fi

if [ -z "${ARCANE_SHADER_TARGETS:-}" ]; then
    case "$(uname -s)" in
        Darwin) ARCANE_SHADER_TARGETS=spirv ;;
        *)      ARCANE_SHADER_TARGETS="dxil spirv" ;;
    esac
fi
WANT_DXIL=0
WANT_SPIRV=0
for t in $ARCANE_SHADER_TARGETS; do
    case "$t" in
        dxil)  WANT_DXIL=1 ;;
        spirv) WANT_SPIRV=1 ;;
        *) echo "compile-shaders.sh: unknown shader target '$t' (dxil, spirv)" >&2; exit 1 ;;
    esac
done

mkdir -p "$OUT/dxil" "$OUT/spirv"

# `set SPIRV_FLAGS=...` -> the flag string (CR stripped: the .bat is CRLF-safe).
SPIRV_FLAGS=$(tr -d '\r' < "$BAT" | sed -n 's/^set SPIRV_FLAGS=//p')
if [ -z "$SPIRV_FLAGS" ]; then
    echo "compile-shaders.sh: could not read SPIRV_FLAGS from $BAT" >&2
    exit 1
fi

count=0
# `call :compile sprite  vs_main vs_6_5 sprite_vs  || exit /b 1`
tr -d '\r' < "$BAT" | sed -n 's/^call :compile[[:space:]]\{1,\}\([^|]*\)||.*$/\1/p' > "$OUT/.entries"
while read -r src entry profile out; do
    [ -n "$src" ] || continue
    # shellcheck disable=SC2086 -- SPIRV_FLAGS is a word list on purpose.
    if [ "$WANT_DXIL" = 1 ]; then
        "$DXC" -T "$profile" -E "$entry" -Fo "$OUT/dxil/$out.bin" "$SRC/$src.hlsl"
    fi
    if [ "$WANT_SPIRV" = 1 ]; then
        "$DXC" -T "$profile" -E "$entry" $SPIRV_FLAGS -Fo "$OUT/spirv/$out.bin" "$SRC/$src.hlsl"
    fi
    count=$((count + 1))
done < "$OUT/.entries"
rm -f "$OUT/.entries"

if [ "$count" -eq 0 ]; then
    echo "compile-shaders.sh: no 'call :compile' lines found in $BAT" >&2
    exit 1
fi
echo "Shaders compiled to $OUT ($count entry points; targets: $ARCANE_SHADER_TARGETS)"
