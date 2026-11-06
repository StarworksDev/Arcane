#!/usr/bin/env bash
# build.sh <Debug|Release|Dist> [-j N] -- THE Linux/macOS build sequence:
# .github/workflows/ci.yml and scripts/ci/local.ps1 run exactly this. The
# Windows twin is scripts/ci/build.ps1 (same steps, same log names).
#   1. generate  premake5 --cc=<gcc|clang> gmake      (family from $CC)
#   2. engine    make -k  ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient ArcanePhysics2D arcane-abi-digest
#      abi       arcane-abi-digest --check (build/abi-digest.txt + kGamePluginABIDigest)
#   3. game      arcbuild build --project ReferenceProject --config <cfg> --sdk <checkout>
#   4. hosts     make -k  ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
#                (their postbuild stages ReferenceProject, Binaries/ included)
# Stops at the first failing step and exits with its code (make -k inside a
# step still reports every error of that step). Logs:
# logs/build-{generate,engine,referencegame,tests}.log; stale ones go first.
# Jobs: -j N, else $MAKE_JOBS, else the CPU count.
#
# Tools: with ARCANE_TOOLS_DIR set (the CI image), premake5 and the Linux DXC
# are symlinked from it into ThirdParty/ (an existing real file or directory
# there is kept). Otherwise they must already be in place
# (scripts/fetch-premake.sh; scripts/fetch-dxc-linux.sh or
# scripts/fetch-vulkan-sdk-macos.sh).
#
# Portability: bash 3.2 (macOS /bin/bash) + POSIX tools -- no associative
# arrays, case-modifying expansions or mapfile. No git: desk checkouts are
# worktrees whose .git file points at a Windows path that does not exist in
# the container; revision info, if ever needed, comes in through the env.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
usage() { echo "usage: build.sh <Debug|Release|Dist> [-j N]" >&2; exit 2; }

CONFIG="${1:-}"
[ $# -gt 0 ] && shift
case "$CONFIG" in Debug|Release|Dist) ;; *) usage ;; esac
JOBS="${MAKE_JOBS:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        -j) [ $# -ge 2 ] || usage; JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        *) usage ;;
    esac
done
if [ -z "$JOBS" ]; then JOBS=$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 2); fi
case "$JOBS" in ''|*[!0-9]*) echo "build: -j needs a number (got '$JOBS')" >&2; exit 2 ;; esac

case "${CC:-}" in
    *clang*) PREMAKE_CC=clang ;;
    *gcc*|*g++*) PREMAKE_CC=gcc ;;
    '') if [ "$(uname -s)" = Darwin ]; then PREMAKE_CC=clang; else PREMAKE_CC=gcc; fi ;;
    *) echo "build: cannot tell gcc from clang in CC='$CC'" >&2; exit 2 ;;
esac

cd "$ROOT" || exit 2

link_tool() {   # link_tool <checkout path> <tool path>
    local dest=$1 src=$2
    if [ -L "$dest" ] || [ ! -e "$dest" ]; then
        mkdir -p "$(dirname "$dest")"
        ln -sfn "$src" "$dest"
    else
        echo "build: keeping the existing $dest (not linking $src)"
    fi
}
if [ -n "${ARCANE_TOOLS_DIR:-}" ]; then
    link_tool ThirdParty/premake5/premake5 "$ARCANE_TOOLS_DIR/premake5/premake5"
    link_tool ThirdParty/tools/dxc-linux "$ARCANE_TOOLS_DIR/dxc-linux"
fi
if [ ! -x ThirdParty/premake5/premake5 ]; then
    echo "build: ThirdParty/premake5/premake5 is missing -- run scripts/fetch-premake.sh (the CI image sets ARCANE_TOOLS_DIR instead)" >&2
    exit 2
fi

mkdir -p logs
rm -f logs/build-*.log
config_lc=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
bindir=$(scripts/arcane-bin-dir.sh "$CONFIG") || exit 2

step() {   # step <name> <log> <command...>
    local name=$1 log=$2
    shift 2
    echo "build: [$name] $*"
    "$@" 2>&1 | tee "logs/$log"
    local rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        echo "build: [$name] FAILED (exit $rc) -- logs/$log" >&2
        exit "$rc"
    fi
}

step generate build-generate.log ThirdParty/premake5/premake5 --cc="$PREMAKE_CC" gmake
step engine build-engine.log make -k -j"$JOBS" config="$config_lc" \
    ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient ArcanePhysics2D arcane-abi-digest
step abi build-abi.log "$bindir/arcane-abi-digest/arcane-abi-digest" --check --root "$ROOT"
step game build-referencegame.log "$bindir/arcbuild/arcbuild" build --project ReferenceProject \
    --config "$CONFIG" --sdk "$ROOT"
ls -l ReferenceProject/Binaries/
step hosts build-tests.log make -k -j"$JOBS" config="$config_lc" \
    ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests
echo "build: $CONFIG ok"
