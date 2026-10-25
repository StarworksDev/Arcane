#!/usr/bin/env bash
# Build and run the libFuzzer harnesses under fuzz/. See docs/fuzzing.md.
#
#   scripts/run-fuzz.sh <target|all> [seconds]        build + fuzz (default 60s each)
#   scripts/run-fuzz.sh build <target|all>            build only
#   scripts/run-fuzz.sh regress <target|all>          replay fuzz/regressions + corpus, no fuzzing
#   scripts/run-fuzz.sh coverage <target> [dir...]    line coverage of the engine sources the
#                                                     target reaches, over its corpus (+ dirs)
#
# Targets: protocol artifact scene sprite cli
#
# Environment:
#   CXX          clang++ with libFuzzer (default: clang++-19, else clang++)
#   FUZZ_JOBS    parallel workers per target (default 1)
#   FUZZ_FLAGS   extra libFuzzer flags (e.g. "-dict=... -only_ascii=1")
#   FUZZ_OUT     build/work dir (default fuzz/build, gitignored)
#   LLVM_COV / LLVM_PROFDATA   coverage tools matching $CXX (default llvm-cov-19 / llvm-profdata-19)
#
# Each harness compiles standalone against the Runtime module headers plus only the
# .cpp files it needs -- no premake, no engine build -- so this works from
# `main` on any Linux box with clang + compiler-rt (libclang-rt-19-dev).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${FUZZ_OUT:-$ROOT/fuzz/build}"
CXX="${CXX:-$(command -v clang++-19 || command -v clang++)}"
ALL_TARGETS=(protocol artifact scene sprite cli)

# Every Runtime module's include root (Source/Runtime/<Module>/src).
INCLUDES=()
for d in "$ROOT"/Source/Runtime/*/src; do INCLUDES+=(-I"$d"); done
INCLUDES+=(
    -I"$ROOT/ThirdParty/nlohmann"
    -I"$ROOT/ThirdParty/spdlog/include"
    -I"$ROOT/ThirdParty/glm"
    -I"$ROOT/ThirdParty/Astra/include"
    -I"$ROOT/ThirdParty/Mosaic/include"
    -I"$ROOT/ThirdParty/picosha2"
    -I"$ROOT/ThirdParty/Manifold2D/include"
)
CXXFLAGS=(-std=c++23 -g -O1 -fno-omit-frame-pointer
          -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
          -DARCANE_FUZZING=1 -w)

# The engine sources each target links besides its harness (relative to the repo root).
sources_for() {
    case "$1" in
        protocol) echo "Source/Runtime/ArcaneCore/src/Arcane/Config/CVarRegistry.cpp Source/Runtime/ArcaneCore/src/Arcane/Config/CVarFormat.cpp Source/Runtime/ArcaneCore/src/Arcane/Base/Assert.cpp fuzz/support/DiagnosticsStub.cpp fuzz/support/LogStub.cpp" ;;
        artifact) echo "Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactReader.cpp Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactCodec.cpp Source/Runtime/ArcaneCore/src/Arcane/Guid.cpp Source/Runtime/ArcaneCore/src/Arcane/IO/ReadRegularFile.cpp Source/Runtime/ArcaneCore/src/Arcane/Platform/SecureRandom.cpp fuzz/support/LogStub.cpp" ;;
        scene)    echo "Source/Runtime/ArcaneCore/src/Arcane/Guid.cpp Source/Runtime/ArcaneCore/src/Arcane/Platform/SecureRandom.cpp fuzz/support/DiagnosticsStub.cpp fuzz/support/LogStub.cpp" ;;
        sprite)   echo "Source/Runtime/ArcaneAssets/src/Arcane/Sprite/SpriteAsset.cpp Source/Runtime/ArcaneCore/src/Arcane/Guid.cpp Source/Runtime/ArcaneCore/src/Arcane/Platform/SecureRandom.cpp fuzz/support/LogStub.cpp" ;;
        cli)      echo "Source/Runtime/ArcaneCore/src/Arcane/Cli/Cli.cpp Source/Runtime/ArcaneCore/src/Arcane/Config/CVarRegistry.cpp Source/Runtime/ArcaneCore/src/Arcane/Config/CVarFormat.cpp Source/Runtime/ArcaneCore/src/Arcane/Base/Assert.cpp fuzz/support/DiagnosticsStub.cpp fuzz/support/LogStub.cpp Source/Programs/ArcaneServer/src/ServerConfig.cpp" ;;
        *) echo "unknown target '$1' (known: ${ALL_TARGETS[*]})" >&2; return 1 ;;
    esac
}

# -max_len per format: big enough for every seed plus room to grow, small
# enough that the fuzzer spends its time on structure, not on size.
maxlen_for() {
    case "$1" in
        protocol) echo 16384 ;;   # below net.maxReceiveBufferBytes (64 KiB) -- see protocol_fuzz.cpp
        artifact) echo 8192 ;;
        scene)    echo 16384 ;;
        sprite)   echo 4096 ;;
        cli)      echo 1024 ;;
    esac
}

build() {
    local t="$1"
    local srcs; srcs="$(sources_for "$t")"
    mkdir -p "$OUT"
    local abs=()
    for s in $srcs; do abs+=("$ROOT/$s"); done
    echo "[run-fuzz] building $t with $CXX" >&2
    local log="$OUT/build-$t.log"
    if ! "$CXX" "${CXXFLAGS[@]}" "${INCLUDES[@]}" "$ROOT/fuzz/${t}_fuzz.cpp" "${abs[@]}" \
        -o "$OUT/${t}_fuzz" >"$log" 2>&1; then
        cat "$log" >&2
        # The source lists in sources_for() are hand-kept; name the usual cause.
        if grep -q "undefined (reference|symbol)" -E "$log"; then
            echo "[run-fuzz] $t: LINK FAILED on undefined symbols -- sources_for() is stale; add the .cpp (or a fuzz/support stub) defining:" >&2
            grep -oE "undefined (reference to|symbol:) .*" "$log" | sort -u | head -20 >&2
        fi
        return 1
    fi
}

# Per-target libFuzzer flags. The Cli parser prints usage on every rejected
# argv; muting the harness's stdout/stderr keeps it fast (crash reports still
# come through: libFuzzer restores stderr before it dies).
extra_args_for() {
    case "$1" in
        cli) echo "-close_fd_mask=3" ;;
    esac
    return 0
}

dict_args() {
    local d="$ROOT/fuzz/dict/$1.dict"
    [[ -f "$d" ]] && echo "-dict=$d"
    return 0
}

run() {
    local t="$1" secs="$2"
    build "$t"
    local work="$OUT/corpus/$t"
    mkdir -p "$work" "$OUT/artifacts/$t"
    # Fuzz into a scratch corpus dir seeded from the checked-in seeds; the
    # checked-in corpus stays small and hand-made.
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:symbolize=1}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
    local par=()
    # -jobs N>1 makes libFuzzer run N independent workers, each logging to
    # fuzz-<n>.log in the CWD -- so run from the work dir.
    if [[ "${FUZZ_JOBS:-1}" -gt 1 ]]; then par=(-jobs="$FUZZ_JOBS" -workers="$FUZZ_JOBS"); fi
    # shellcheck disable=SC2046
    (cd "$OUT" && "$OUT/${t}_fuzz" "$work" "$ROOT/fuzz/corpus/$t" \
        -max_total_time="$secs" -max_len="$(maxlen_for "$t")" -timeout=10 -rss_limit_mb=2048 \
        -artifact_prefix="$OUT/artifacts/$t/" -print_final_stats=1 \
        "${par[@]}" $(dict_args "$t") $(extra_args_for "$t") ${FUZZ_FLAGS:-})
}

regress() {
    local t="$1"
    build "$t"
    local inputs=()
    for d in "$ROOT/fuzz/regressions/$t" "$ROOT/fuzz/corpus/$t"; do
        [[ -d "$d" ]] && while IFS= read -r -d '' f; do inputs+=("$f"); done < <(find "$d" -type f ! -name '*.md' -print0)
    done
    if [[ ${#inputs[@]} -eq 0 ]]; then echo "[run-fuzz] $t: no inputs" >&2; return 0; fi
    # Running the binary on files (not dirs) executes each once and exits non-zero on the first failure.
    "$OUT/${t}_fuzz" -runs=1 "${inputs[@]}" >/dev/null 2>"$OUT/regress-$t.log" \
        || { cat "$OUT/regress-$t.log" >&2; echo "[run-fuzz] $t: REGRESSION FAILED" >&2; return 1; }
    echo "[run-fuzz] $t: ${#inputs[@]} inputs OK" >&2
}

# Source-based coverage (no sanitizers) of the Runtime-module files a target
# exercises, from replaying a corpus once. Reported per engine file.
coverage() {
    local t="$1"; shift
    local srcs; srcs="$(sources_for "$t")"
    local abs=()
    for s in $srcs; do abs+=("$ROOT/$s"); done
    mkdir -p "$OUT/cov"
    "$CXX" -std=c++23 -g -O0 -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping \
        -DARCANE_FUZZING=1 -w "${INCLUDES[@]}" "$ROOT/fuzz/${t}_fuzz.cpp" "${abs[@]}" -o "$OUT/cov/${t}_cov"
    local dirs=("$ROOT/fuzz/corpus/$t" "$@")
    [[ -d "$OUT/corpus/$t" ]] && dirs+=("$OUT/corpus/$t")
    [[ -d "$ROOT/fuzz/regressions/$t" ]] && dirs+=("$ROOT/fuzz/regressions/$t")
    rm -f "$OUT/cov/$t".*.profraw
    # -runs=0 executes every corpus input once and exits.
    (cd "$OUT/cov" && LLVM_PROFILE_FILE="$OUT/cov/$t.%p.profraw" ./"${t}_cov" -runs=0 "${dirs[@]}" >/dev/null 2>&1) || true
    "${LLVM_PROFDATA:-llvm-profdata-19}" merge -sparse "$OUT/cov/$t".*.profraw -o "$OUT/cov/$t.profdata"
    "${LLVM_COV:-llvm-cov-19}" report "$OUT/cov/${t}_cov" -instr-profile="$OUT/cov/$t.profdata" \
        "$ROOT"/Source/Runtime/*/src/Arcane "$ROOT/Source/Programs/ArcaneServer/src"
}

targets_of() { if [[ "$1" == all ]]; then echo "${ALL_TARGETS[@]}"; else echo "$1"; fi; }

case "${1:-}" in
    ""|-h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    build)   for t in $(targets_of "${2:?target}"); do build "$t"; done ;;
    coverage) t="${2:?target}"; shift 2; coverage "$t" "$@" ;;
    regress) rc=0; for t in $(targets_of "${2:?target}"); do regress "$t" || rc=1; done; exit $rc ;;
    *)       for t in $(targets_of "$1"); do run "$t" "${2:-60}"; done ;;
esac
