#!/usr/bin/env bash
# The ONE ArcaneTests entry point on Linux and macOS -- CI (.github/workflows/
# ci.yml) and a developer run exactly this. Windows' twin is scripts/run-tests.ps1
# (same arguments).
#
#   scripts/run-tests.sh [--gpu] <Config> [--rng-seed N] [extra Catch2 args...]
#     Config: Debug | Release | Dist   (bin/<Config>-<system>-<arch>-md/ArcaneTests,
#             see scripts/arcane-bin-dir.sh)
#     --rng-seed N: ArcaneTests runs in RANDOM order; CI runs every leg with two
#             seeds. Omitted -> Catch2 picks one and prints it in the banner.
#     --gpu:  run "[gpu]" instead of "~[gpu]" (same exclusions). Needs a Vulkan
#             ICD (Linux CI: Mesa lavapipe; macOS: MoltenVK) and, for windowed
#             cases, a display.
#
# Runs FROM the exe directory (data paths are exe-relative), headless by
# default (SDL_VIDEODRIVER=offscreen unless the caller set one), with every
# case in this OS's exclusion list removed:
#   Linux: scripts/linux-test-exclusions.txt    macOS: scripts/macos-test-exclusions.txt
# (ARCANE_TEST_EXCLUSIONS overrides). Format: one case per line,
# "<exact test case name> | <reason>"; '#' lines and blanks are ignored; a line
# without a reason fails this script, so a list cannot grow unexplained.
#
# Reports: console, plus JUnit at test-results/<leg>-<Config>[-gpu]-seed<N>.junit.xml
# (<leg> = $ARCANE_CI_LEG, default the OS name). Exit status is ArcaneTests'.
#
# Catch2 ANDs separate test-spec arguments, so each exclusion is passed as its
# own ~"name" argument. Inside a quoted name Catch2 still treats ',' as an OR
# separator and '\' as an escape, so both (and '"') are backslash-escaped here.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GPU_SPEC="~[gpu]"
GPU_TAG=""
if [ "${1:-}" = "--gpu" ]; then
    GPU_SPEC="[gpu]"
    GPU_TAG="-gpu"
    shift
fi
CONFIG="${1:-Debug}"
shift || true
case "$CONFIG" in Debug|Release|Dist) ;; *) echo "run-tests: config must be Debug, Release or Dist (got '$CONFIG')" >&2; exit 2 ;; esac

SEED=""
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --rng-seed) SEED="${2:?--rng-seed needs a value}"; args+=("--rng-seed" "$SEED"); shift 2 ;;
        --rng-seed=*) SEED="${1#--rng-seed=}"; args+=("--rng-seed" "$SEED"); shift ;;
        *) args+=("$1"); shift ;;
    esac
done

case "$(uname -s)" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    *) echo "run-tests: unsupported host $(uname -s)" >&2; exit 2 ;;
esac
EXE_DIR="$ROOT/$("$ROOT/scripts/arcane-bin-dir.sh" "$CONFIG")/ArcaneTests"
EXCLUSIONS="${ARCANE_TEST_EXCLUSIONS:-$ROOT/scripts/$OS-test-exclusions.txt}"

if [ ! -x "$EXE_DIR/ArcaneTests" ]; then
    echo "run-tests: $EXE_DIR/ArcaneTests not built" >&2
    exit 2
fi

specs=("$GPU_SPEC")
if [ -f "$EXCLUSIONS" ]; then
    lineno=0
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        case "$line" in ''|'#'*) continue ;; esac
        name="${line%% | *}"
        reason="${line#* | }"
        if [ "$name" = "$line" ] || [ -z "${reason// /}" ]; then
            echo "run-tests: $EXCLUSIONS:$lineno has no ' | reason': $line" >&2
            exit 2
        fi
        name="${name%"${name##*[![:space:]]}"}"   # trim trailing blanks
        esc=${name//\\/\\\\}
        esc=${esc//,/\\,}
        esc=${esc//\"/\\\"}
        specs+=("~\"$esc\"")
    done < "$EXCLUSIONS"
fi

LEG="${ARCANE_CI_LEG:-$OS}"
mkdir -p "$ROOT/test-results"
JUNIT="$ROOT/test-results/$LEG-$CONFIG$GPU_TAG-seed${SEED:-random}.junit.xml"

echo "run-tests: $OS $CONFIG, seed ${SEED:-random}, ${#specs[@]} spec(s) ($GPU_SPEC + $(( ${#specs[@]} - 1 )) exclusion(s)) -> $JUNIT"
cd "$EXE_DIR"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
exec ./ArcaneTests "${specs[@]}" "${args[@]+"${args[@]}"}" -r console -r "junit::out=$JUNIT"
