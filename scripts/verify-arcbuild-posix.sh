#!/bin/sh
# verify-arcbuild-posix.sh -- the POSIX side of arcbuild's process runner
# (multibackend hardening Task 6, spec docs/specs/
# 2026-09-20-arcbuild-multibackend-hardening-design.md s6.4).
#
# arcbuild's ProcessRunner has two branches: CreateProcessW under _WIN32, and
# pipe/fork/dup2/chdir/execv/waitpid outside it. A Windows desk can build and
# run only the first, so this script is what a Linux/macOS host runs to
# validate the second. Deliberately /bin/sh (not bash): it must run on a bare
# CI container and on macOS's own shell without extra packages.
#
# Two stages, in increasing order of what they need from the host:
#
#   Stage 1 -- the PORTABLE COMPILE CONTRACT. Compiles the process-runner
#     translation unit and its arcbuild-local closure with a real POSIX
#     compiler. Process.cpp's include closure is arcbuild-local plus standard
#     headers only (no ArcaneCore, no engine, no third party), so this stage
#     needs nothing but a C++23 compiler and is the one stage that is expected
#     to pass TODAY. It is what proves the POSIX branch is real code the
#     platform's compiler accepts -- headers guarded correctly, no Win32
#     symbol leaking out of _WIN32, no POSIX header hiding inside it.
#
#   Stage 2 -- GENERATE, BUILD, TEST. premake5 gmake, then make arcbuild and
#     ArcaneTests, then ArcaneTests '[build]' (whose POSIX cases spawn /bin/sh
#     through the real runner: normal exit, 128 + signal, missing executable,
#     missing working directory, merged stdout/stderr, and argv passed through
#     unchanged). This stage needs the Arcane Linux port (2026-10-05): a
#     system SDL3 (pkg-config sdl3) and the fetched Linux DXC
#     (scripts/fetch-dxc-linux.sh) for the shader prebuild. It fails loudly
#     rather than being skipped: a green run of this script means both stages
#     passed, and nothing weaker.
#
# Until stage 2 can run, the POSIX runner's RUNTIME behavior was validated by
# compiling Process.cpp/Compose.cpp/Output.cpp together with a throwaway main()
# that reproduces the [build] POSIX cases (BuildDriverTest.cpp) -- normal exit,
# 128 + signal, missing executable, missing working directory, merged
# stdout/stderr, argv passed through unchanged -- and running it on Linux. Task
# 6's report records that run; reproduce it the same way if this script's stage
# 2 is still blocked.
#
# Usage:
#   scripts/verify-arcbuild-posix.sh              # both stages
#   scripts/verify-arcbuild-posix.sh --syntax-only  # stage 1 only
#
# Environment:
#   CXX          compiler for stage 1 (default: c++)
#   PREMAKE5     premake5 binary for stage 2 (default: ThirdParty/premake5/premake5
#                when scripts/fetch-premake-linux.sh has run, else premake5 on PATH)
#   MAKE         make binary for stage 2 (default: make)
#   CONFIG       premake configuration for stage 2 (default: Debug)
#   PREMAKE_ACTION  premake action for stage 2 (default: gmake)

set -eu

case "$(uname -s)" in
    Linux|Darwin|*BSD) ;;
    *)
        echo "verify-arcbuild-posix.sh: this script is for Linux/macOS/BSD hosts;" \
             "'$(uname -s)' is not one. On Windows, run the msbuild + ArcaneTests" \
             "'[build]' regression instead." >&2
        exit 1
        ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$repo_root"

syntax_only=0
case "$#:${1:-}" in
    0:) ;;
    1:--syntax-only) syntax_only=1 ;;
    *)
        echo "usage: $0 [--syntax-only]" >&2
        echo "  (unrecognized argument(s): $*)" >&2
        exit 2
        ;;
esac

CXX=${CXX:-c++}
if [ -z "${PREMAKE5:-}" ] && [ -x "ThirdParty/premake5/premake5" ]; then
    PREMAKE5=ThirdParty/premake5/premake5
fi
PREMAKE5=${PREMAKE5:-premake5}
MAKE=${MAKE:-make}
CONFIG=${CONFIG:-Debug}
# `gmake` is the action this repo's generation contract names; a premake build
# that only knows the newer `gmake2` spelling can be told so without editing
# this script.
PREMAKE_ACTION=${PREMAKE_ACTION:-gmake}

echo "=== arcbuild POSIX verification ============================================"
echo "host    : $(uname -s) $(uname -m)"
echo "repo    : $repo_root"
echo "compiler: $CXX"
echo

# ---- Stage 1: the portable compile contract --------------------------------
#
# -Wall -Wextra -Werror on purpose: an unused variable or a fallen-off return
# in a branch no Windows compiler ever sees is exactly the class of defect this
# stage exists to catch. Process.cpp is the Task 6 subject; Compose.cpp and
# Output.cpp are the rest of the runner's own arcbuild-local closure.
echo "--- stage 1: POSIX compile of the process runner ---------------------------"

for unit in arcbuild/src/Process.cpp arcbuild/src/Compose.cpp arcbuild/src/Output.cpp; do
    echo "  $CXX -std=c++23 -fsyntax-only $unit"
    "$CXX" -std=c++23 -Wall -Wextra -Werror -fsyntax-only \
        -Iarcbuild/src "$unit"
done

echo "  OK: the POSIX branch compiles clean with no Win32 header in sight."
echo

if [ "$syntax_only" -eq 1 ]; then
    echo "--syntax-only: stopping before generation/build/test."
    exit 0
fi

# ---- Stage 2: generate, build, run the [build] suite -----------------------
echo "--- stage 2: premake gmake + build + ArcaneTests '[build]' -----------------"

if ! command -v "$PREMAKE5" > /dev/null 2>&1; then
    echo "verify-arcbuild-posix.sh: premake5 not found (set PREMAKE5 to a POSIX" \
         "premake5 build; ThirdParty/premake5/ ships the Windows .exe only)." >&2
    exit 1
fi

if ! command -v "$MAKE" > /dev/null 2>&1; then
    echo "verify-arcbuild-posix.sh: make not found (set MAKE)." >&2
    exit 1
fi

# Off-Windows premake5.lua takes SDL3 from the system (pkg-config sdl3 or
# SDL3_ROOT), not vcpkg -- VCPKG_ROOT is a Windows-target requirement only.
if ! pkg-config --exists sdl3 2>/dev/null && [ -z "${SDL3_ROOT:-}" ]; then
    echo "verify-arcbuild-posix.sh: SDL3 not found (pkg-config sdl3, or set SDL3_ROOT)." >&2
    exit 1
fi

echo "  $PREMAKE5 $PREMAKE_ACTION"
"$PREMAKE5" "$PREMAKE_ACTION"

make_config=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
echo "  $MAKE config=$make_config arcbuild ArcaneTests"
"$MAKE" config="$make_config" arcbuild ArcaneTests

# premake's outputdir is "<Config>-<cfg.system>-<cfg.architecture>-md", and
# cfg.system spells macOS "macosx" (never uname's "Darwin"); the workspace
# pins architecture "x64", which premake reports as "x86_64".
case "$(uname -s)" in
    Linux)  premake_system=linux ;;
    Darwin) premake_system=macosx ;;
    *)      premake_system=$(uname -s | tr '[:upper:]' '[:lower:]') ;;
esac

# The test exe resolves its fixtures relative to its own directory, so it is
# run FROM that directory -- the same rule the Windows runs follow.
test_dir="bin/$CONFIG-$premake_system-x86_64-md/ArcaneTests"

if [ ! -x "$test_dir/ArcaneTests" ]; then
    echo "verify-arcbuild-posix.sh: built ArcaneTests not found at $test_dir --" \
         "adjust the output-directory guess in this script to match premake's" \
         "outputdir for this host." >&2
    exit 1
fi

echo "  (cd $test_dir && ./ArcaneTests '[build]')"
( cd "$test_dir" && ./ArcaneTests "[build]" )

echo
echo "=== arcbuild POSIX verification: PASS ======================================"
