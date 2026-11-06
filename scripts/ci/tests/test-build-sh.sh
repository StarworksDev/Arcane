#!/usr/bin/env bash
# Tests for scripts/ci/build.sh against a fake checkout: stub premake5, make,
# arcane-abi-digest and arcbuild record their arguments, so the step order, flags, logs and the
# stop-at-first-failure rule are checked without a compiler. Runs in the
# image-tag job of .github/workflows/ci.yml and on any Linux/macOS shell.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }

# stub <path> <name>: records "<name> <args>" in $CALLS. premake exits
# $FAIL_PREMAKE when set; make exits $FAIL_CODE (default 3) when its
# arguments contain the word $FAIL_MAKE_ON; the digest tool exits $FAIL_ABI
# when set.
stub() {
    cat > "$1" <<EOF
#!/bin/sh
echo "$2 \$*" >> "\$CALLS"
if [ "$2" = make ] && [ -n "\${FAIL_MAKE_ON:-}" ]; then
    case " \$* " in *" \$FAIL_MAKE_ON "*) exit "\${FAIL_CODE:-3}" ;; esac
fi
if [ "$2" = premake ] && [ -n "\${FAIL_PREMAKE:-}" ]; then exit "\$FAIL_PREMAKE"; fi
if [ "$2" = abi ] && [ -n "\${FAIL_ABI:-}" ]; then exit "\$FAIL_ABI"; fi
exit 0
EOF
    chmod +x "$1"
}

# fixture [--no-premake]: prints a fake checkout root.
fixture() {
    local d c bin abi
    d=$(mktemp -d)
    mkdir -p "$d/scripts/ci" "$d/ThirdParty/premake5" "$d/ReferenceProject/Binaries" "$d/fakebin"
    cp "$REPO/scripts/ci/build.sh" "$d/scripts/ci/build.sh"
    cp "$REPO/scripts/arcane-bin-dir.sh" "$d/scripts/arcane-bin-dir.sh"
    chmod +x "$d/scripts/ci/build.sh" "$d/scripts/arcane-bin-dir.sh"
    if [ "${1:-}" != --no-premake ]; then stub "$d/ThirdParty/premake5/premake5" premake; fi
    stub "$d/fakebin/make" make
    for c in Debug Release Dist; do
        bin="$d/$("$d/scripts/arcane-bin-dir.sh" "$c")/arcbuild"
        mkdir -p "$bin"
        stub "$bin/arcbuild" arcbuild
        abi="$d/$("$d/scripts/arcane-bin-dir.sh" "$c")/arcane-abi-digest"
        mkdir -p "$abi"
        stub "$abi/arcane-abi-digest" abi
    done
    echo "$d"
}

# run <root> [VAR=value...] <command...>: sets RC; $root/calls.txt, $root/out.txt.
run() {
    local root=$1; shift
    : > "$root/calls.txt"
    set +e
    (cd "$root" && env -u CC -u CXX -u MAKE_JOBS -u ARCANE_TOOLS_DIR \
        PATH="$root/fakebin:$PATH" CALLS="$root/calls.txt" "$@") > "$root/out.txt" 2>&1
    RC=$?
    set -e
}
first_call() { head -n 1 "$1/calls.txt"; }

d=$(fixture); run "$d" CC=gcc-14 scripts/ci/build.sh Debug -j 4
expected="premake --cc=gcc gmake
make -k -j4 config=debug ArcaneCore ArcaneAssetPipeline arccook arcbuild ArcaneClient ArcanePhysics2D arcane-abi-digest
abi --check --root $d
arcbuild build --project ReferenceProject --config Debug --sdk $d
make -k -j4 config=debug ArcaneServer ArcaneRuntime ArcaneEditor ArcaneTests"
[ "$RC" -eq 0 ] && [ "$(cat "$d/calls.txt")" = "$expected" ] && ok "steps, order and flags" \
    || bad "steps, order and flags (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"
all=1; for l in generate engine abi referencegame tests; do [ -f "$d/logs/build-$l.log" ] || all=0; done
[ "$all" -eq 1 ] && ok "five step logs" || bad "five step logs: $(ls "$d/logs" 2>&1)"

d=$(fixture); run "$d" CC=clang-19 scripts/ci/build.sh Release
[ "$(first_call "$d")" = "premake --cc=clang gmake" ] && ok "clang from CC" || bad "clang from CC: $(first_call "$d")"
grep -q "config=release" "$d/calls.txt" && ok "config lower-cased for make" || bad "config lower-cased for make"

d=$(fixture); run "$d" MAKE_JOBS=5 scripts/ci/build.sh Dist
grep -q -- "-j5 config=dist" "$d/calls.txt" && ok "MAKE_JOBS is the default -j" || bad "MAKE_JOBS is the default -j: $(cat "$d/calls.txt")"

d=$(fixture); run "$d" MAKE_JOBS=5 scripts/ci/build.sh Debug -j7
grep -q -- "-j7 config=debug" "$d/calls.txt" && ok "-jN beats MAKE_JOBS" || bad "-jN beats MAKE_JOBS"

if [ "$(uname -s)" = Linux ]; then
    d=$(fixture); run "$d" scripts/ci/build.sh Debug
    [ "$(first_call "$d")" = "premake --cc=gcc gmake" ] && ok "no CC on Linux -> gcc" || bad "no CC on Linux -> gcc"
fi

d=$(fixture); run "$d" FAIL_MAKE_ON=ArcaneCore FAIL_CODE=3 scripts/ci/build.sh Debug
[ "$RC" -eq 3 ] && ! grep -q '^arcbuild' "$d/calls.txt" && [ ! -f "$d/logs/build-tests.log" ] \
    && ok "engine failure stops the build with its code" || bad "engine failure stops the build (rc $RC): $(cat "$d/calls.txt")"

d=$(fixture); run "$d" FAIL_ABI=5 scripts/ci/build.sh Debug
[ "$RC" -eq 5 ] && ! grep -q '^arcbuild' "$d/calls.txt" && grep -q "\[abi\] FAILED" "$d/out.txt" \
    && ok "abi digest mismatch stops the build before the game" || bad "abi digest mismatch stops the build (rc $RC): $(cat "$d/calls.txt")"

d=$(fixture); run "$d" FAIL_PREMAKE=4 scripts/ci/build.sh Debug
[ "$RC" -eq 4 ] && [ "$(wc -l < "$d/calls.txt" | tr -d ' ')" = 1 ] && ok "generate failure stops the build" || bad "generate failure stops the build (rc $RC)"

d=$(fixture); mkdir -p "$d/logs"; echo stale > "$d/logs/build-tests.log"
run "$d" FAIL_PREMAKE=4 scripts/ci/build.sh Debug
[ ! -f "$d/logs/build-tests.log" ] && ok "stale step logs removed" || bad "stale step logs removed"

d=$(fixture); run "$d" scripts/ci/build.sh debug
[ "$RC" -eq 2 ] && [ ! -s "$d/calls.txt" ] && ok "bad config -> 2" || bad "bad config -> 2 (rc $RC)"
d=$(fixture); run "$d" scripts/ci/build.sh Debug -j x
[ "$RC" -eq 2 ] && ok "bad -j -> 2" || bad "bad -j -> 2 (rc $RC)"
d=$(fixture); run "$d" CC=icc scripts/ci/build.sh Debug
[ "$RC" -eq 2 ] && ok "unknown compiler family -> 2" || bad "unknown compiler family -> 2 (rc $RC)"

tools=$(mktemp -d); mkdir -p "$tools/premake5" "$tools/dxc-linux/bin"; stub "$tools/premake5/premake5" premake
d=$(fixture --no-premake); run "$d" ARCANE_TOOLS_DIR="$tools" scripts/ci/build.sh Debug
[ "$RC" -eq 0 ] && [ -L "$d/ThirdParty/premake5/premake5" ] && [ "$(readlink "$d/ThirdParty/tools/dxc-linux")" = "$tools/dxc-linux" ] \
    && ok "ARCANE_TOOLS_DIR linked into ThirdParty" || bad "ARCANE_TOOLS_DIR linked (rc $RC): $(cat "$d/out.txt")"
run "$d" ARCANE_TOOLS_DIR="$tools" scripts/ci/build.sh Debug
[ "$RC" -eq 0 ] && ok "re-linking is idempotent" || bad "re-linking is idempotent (rc $RC)"

d=$(fixture); run "$d" ARCANE_TOOLS_DIR="$tools" scripts/ci/build.sh Debug
[ ! -L "$d/ThirdParty/premake5/premake5" ] && grep -q "keeping the existing" "$d/out.txt" \
    && ok "an existing real premake is kept" || bad "an existing real premake is kept"

d=$(fixture --no-premake); run "$d" scripts/ci/build.sh Debug
[ "$RC" -eq 2 ] && grep -q "fetch-premake.sh" "$d/out.txt" && ok "missing premake names the fix" || bad "missing premake names the fix (rc $RC)"

# Portability lints (comment lines ignored): bash 3.2 has no associative
# arrays, case-modifying expansions, mapfile/readarray, namerefs, |&, coproc.
LINTED="$REPO/scripts/ci/build.sh $REPO/scripts/ci/container-leg.sh"
hits=$(grep -nE 'declare -A|typeset -A|mapfile|readarray|\$\{[A-Za-z_][A-Za-z0-9_]*(,,|\^\^)|local -n|\|&|coproc' $LINTED | grep -vE '^([^:]*:)?[0-9]+:[[:space:]]*#' || true)
[ -z "$hits" ] && ok "bash 3.2 lint" || bad "bash 3.2 lint: $hits"
gits=$(grep -nE '(^|[^A-Za-z0-9_./-])git([[:space:]]|$)' "$REPO/scripts/ci/build.sh" | grep -vE '^[0-9]+:[[:space:]]*#' || true)
[ -z "$gits" ] && ok "build.sh calls no git" || bad "build.sh calls git: $gits"

echo
[ "$fails" -eq 0 ] && echo "test-build-sh: all passed" || echo "test-build-sh: $fails failure(s)"
[ "$fails" -eq 0 ]
