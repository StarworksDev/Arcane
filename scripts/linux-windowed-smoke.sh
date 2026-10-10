#!/usr/bin/env bash
# Windowed smoke for the Linux hosts: ArcaneRuntime and ArcaneEditor open a
# REAL window on ReferenceProject, present N frames through the Vulkan
# swapchain and exit 0 (a host exits nonzero when any validation/render error
# fires -- RenderErrorCount). Run by .github/workflows/linux.yml; runnable as is.
#
#   scripts/linux-windowed-smoke.sh <config> <outdir> [x11|wayland]...
#     config:  Debug | Release | Dist  (bin/<config>-linux-x86_64-md/<Host>)
#     outdir:  logs and screenshots land here
#     drivers: default "x11 wayland"
#
# x11:     needs a display -- run under `xvfb-run -a -s "-screen 0 1920x1080x24"`.
#          The editor is RESIZED twice mid-run (xdotool, swapchain recreation)
#          and the X root window is captured (ImageMagick `import`) when both
#          tools are present.
# wayland: starts its own headless weston (pixman renderer) on a private
#          socket, and captures the compositor output with weston-screenshooter.
#
# Every host also writes its own --screenshot (the rendered frame).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONFIG="${1:?config}"
OUT="${2:?outdir}"
shift 2
DRIVERS=("$@")
[ ${#DRIVERS[@]} -eq 0 ] && DRIVERS=(x11 wayland)

BIN="$ROOT/bin/$CONFIG-linux-x86_64-md"
FRAMES="${SMOKE_FRAMES:-180}"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
failures=0

have() { command -v "$1" >/dev/null 2>&1; }

# run_host <driver> <Host> <tag> [extra args...] -- foreground, bounded.
run_host() {
    local driver=$1 host=$2 tag=$3; shift 3
    local log="$OUT/$tag.log"
    echo "smoke: $tag ($host, SDL_VIDEODRIVER=$driver, $FRAMES frames)"
    set +e
    (cd "$BIN/$host" && SDL_VIDEODRIVER=$driver timeout 600 "./$host" \
        --project ReferenceProject --frames "$FRAMES" --screenshot "$OUT/$tag.png" "$@") \
        > "$log" 2>&1
    local rc=$?
    set -e
    report "$tag" "$rc" "$log"
}

report() {
    local tag=$1 rc=$2 log=$3
    if [ "$rc" -ne 0 ] || [ ! -s "$OUT/$tag.png" ]; then
        echo "smoke: FAIL $tag (exit $rc) -- last log lines:" >&2
        tail -n 25 "$log" >&2 || true
        failures=$((failures + 1))
    else
        echo "smoke: ok   $tag -- $(grep -o 'RenderErrorCount [0-9]* -> [0-9]*' "$log" | tail -n 1)"
    fi
}

smoke_x11() {
    if [ -z "${DISPLAY:-}" ]; then
        echo "smoke: x11 needs DISPLAY (run under xvfb-run)" >&2
        failures=$((failures + 1))
        return
    fi
    run_host x11 ArcaneRuntime runtime-x11

    # The editor, with mid-run resizes and a root-window capture of each size.
    local tag=editor-x11 log="$OUT/editor-x11.log"
    echo "smoke: $tag (ArcaneEditor, SDL_VIDEODRIVER=x11, resize + capture)"
    (cd "$BIN/ArcaneEditor" && SDL_VIDEODRIVER=x11 timeout 600 ./ArcaneEditor \
        --project ReferenceProject --frames $((FRAMES * 6)) --screenshot "$OUT/$tag.png") \
        > "$log" 2>&1 &
    local pid=$!
    if have xdotool && have import; then
        local win=""
        for _ in $(seq 120); do
            win=$(xdotool search --name "Arcane" 2>/dev/null | head -n 1 || true)
            [ -n "$win" ] && break
            sleep 0.5
        done
        if [ -n "$win" ]; then
            sleep 4
            import -window root "$OUT/editor-x11-display.png" || true
            xdotool windowsize "$win" 1600 900 || true; sleep 3
            import -window root "$OUT/editor-x11-resized-1600x900.png" || true
            xdotool windowsize "$win" 900 560 || true; sleep 3
            import -window root "$OUT/editor-x11-resized-900x560.png" || true
        else
            echo "smoke: editor window never appeared" >&2
        fi
    fi
    set +e; wait "$pid"; local rc=$?; set -e
    report "$tag" "$rc" "$log"
}

smoke_wayland() {
    if ! have weston; then
        echo "smoke: wayland needs weston" >&2
        failures=$((failures + 1))
        return
    fi
    local runtime_dir
    runtime_dir=$(mktemp -d)
    chmod 700 "$runtime_dir"
    local socket="arcane-smoke-$$"
    XDG_RUNTIME_DIR=$runtime_dir weston --backend=headless --renderer=pixman --debug \
        --socket="$socket" --width=1920 --height=1080 --idle-time=0 \
        > "$OUT/weston.log" 2>&1 &
    local wpid=$!
    for _ in $(seq 100); do [ -S "$runtime_dir/$socket" ] && break; sleep 0.1; done
    if [ ! -S "$runtime_dir/$socket" ]; then
        echo "smoke: weston did not start -- see $OUT/weston.log" >&2
        failures=$((failures + 1)); kill "$wpid" 2>/dev/null || true; return
    fi
    export XDG_RUNTIME_DIR=$runtime_dir WAYLAND_DISPLAY=$socket

    run_host wayland ArcaneRuntime runtime-wayland

    local tag=editor-wayland log="$OUT/editor-wayland.log"
    echo "smoke: $tag (ArcaneEditor, SDL_VIDEODRIVER=wayland, compositor capture)"
    (cd "$BIN/ArcaneEditor" && SDL_VIDEODRIVER=wayland timeout 600 ./ArcaneEditor \
        --project ReferenceProject --frames $((FRAMES * 4)) --screenshot "$OUT/$tag.png") \
        > "$log" 2>&1 &
    local pid=$!
    if have weston-screenshooter; then
        sleep 10
        (cd "$OUT" && timeout 20 weston-screenshooter >/dev/null 2>&1 \
            && for f in wayland-screenshot-*.png; do [ -e "$f" ] && mv "$f" editor-wayland-display.png; done) || true
    fi
    set +e; wait "$pid"; local rc=$?; set -e
    report "$tag" "$rc" "$log"

    kill "$wpid" 2>/dev/null || true
    wait "$wpid" 2>/dev/null || true
    unset WAYLAND_DISPLAY
    rm -rf "$runtime_dir"
}

for d in "${DRIVERS[@]}"; do
    case "$d" in
        x11)     smoke_x11 ;;
        wayland) smoke_wayland ;;
        *) echo "smoke: unknown driver '$d'" >&2; exit 2 ;;
    esac
done

if [ "$failures" -ne 0 ]; then
    echo "smoke: $failures failure(s)" >&2
    exit 1
fi
echo "smoke: all hosts presented and exited cleanly"
