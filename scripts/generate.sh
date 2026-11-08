#!/bin/sh
# generate.sh -- the Linux/macOS developer twin of scripts\generate.bat: refresh
# the generated engine constants, then generate the gmake projects. The family
# comes from $CC exactly as scripts/ci/build.sh derives it. (CI's build.sh runs
# the constants check and premake itself; it does not call this script.)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case "${CC:-}" in
    *clang*) PREMAKE_CC=clang ;;
    *gcc*|*g++*) PREMAKE_CC=gcc ;;
    '') if [ "$(uname -s)" = Darwin ]; then PREMAKE_CC=clang; else PREMAKE_CC=gcc; fi ;;
    *) echo "generate: cannot tell gcc from clang in CC='$CC'" >&2; exit 2 ;;
esac
cd "$ROOT"
sh scripts/generate-engine-constants.sh
ThirdParty/premake5/premake5 --cc="$PREMAKE_CC" gmake
