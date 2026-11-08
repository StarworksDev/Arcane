#!/usr/bin/env bash
# package.sh <Config> -- the monolithic ReferenceProject package (engine
# modularization s3.3 / s6), staged with `arcbuild package` and run IN PLACE
# from Packages/ with no SDK, no Binaries/ and a bare PATH. Linux (the CI image or a
# bare runner) and macOS; scripts/ci/package.ps1 is the Windows twin. Run after
# scripts/ci/build.sh <Config>. Logs: logs/package*.{log,txt}.
#   Linux: ldd shows no Arcane module and the package's OWN libSDL3 (RUNPATH
#          $ORIGIN), then a windowed boot under Xvfb + lavapipe (--frames 60),
#          then REGISTRAR SURVIVAL: ArcaneRuntime (modular) and the packaged
#          game print the same --print-registry JSON, and it carries the
#          ReferenceGame roster. Registrar survival is LINUX-ONLY (GNU ld
#          --whole-archive); MSVC (/WHOLEARCHIVE + /OPT:REF) and macOS (ld64)
#          have NO such witness in CI yet -- tracked in a follow-up issue.
#   macOS: otool -L shows no Arcane module, then --print-engine-info (the
#          hosted runner's windowed boot is not this lane's job).
#   Both:  HOST ISOLATION: ArcaneServer imports no Client/Developer/Editor
#          module (the list comes from each .arcmodule's "type"), with a
#          positive control that a known-good import (ArcaneEngine) matches.
# MAKE_JOBS (default 2) bounds the package build's make -j.
set -euo pipefail
config=${1:?usage: package.sh <Debug|Release|Dist>}
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
mkdir -p logs
bindir=$(scripts/arcane-bin-dir.sh "$config")
MAKEFLAGS=-j"${MAKE_JOBS:-2}" "$bindir/arcbuild/arcbuild" package --project ReferenceProject \
    --config "$config" --sdk "$root" 2>&1 | tee logs/package.log
case "$(uname -s)" in
    Linux)  os=linux;  plat=Linux ;;
    Darwin) os=macosx; plat=Mac ;;
    *) echo "package.sh: unsupported host $(uname -s)" >&2; exit 2 ;;
esac
pkg="ReferenceProject/Packages/ReferenceProject-$os-$config"
ls -la "$pkg"
# Unreal's layout: the game in ReferenceProject/Binaries/<Linux|Mac>/,
# ReferenceProject.sh at the package root -- the boot goes through it.
exe="$pkg/ReferenceProject/Binaries/$plat/ReferenceProject"
if [ "$plat" = Linux ]; then
    ldd "$exe" | tee logs/package-imports.txt
    if grep -qE "libArcane|not found" logs/package-imports.txt; then
        echo "::error::the package imports an Arcane module or misses a library"; exit 1
    fi
    # Its OWN SDL3: an image/runner install must not satisfy it (a RUNPATH of
    # "RIGIN" -- make ate $O -- once passed only because of /usr/local/lib).
    if ! grep -q "libSDL3.so.0 => $(cd "$(dirname "$exe")" && pwd)/" logs/package-imports.txt; then
        echo "::error::the package does not load its own libSDL3 (RUNPATH must start with \$ORIGIN)"; exit 1
    fi
    # XDG_RUNTIME_DIR: as root in the CI container nothing sets it (the image
    # creates /run/user/0); on a bare runner the session's own is kept.
    env -i HOME="$HOME" PATH=/usr/bin:/bin SDL_VIDEODRIVER=x11 \
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
        VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
        xvfb-run -a -s "-screen 0 1280x720x24" "$pkg/ReferenceProject.sh" --frames 60 2>&1 | tee logs/package-boot.log
    # Registrar survival (modularization s6; LINUX ONLY -- see the header): the
    # package boots with the same component and system roster as the modular
    # runtime (--print-registry: one sorted JSON line on stdout). The host's
    # logs go to stderr and xvfb-run may interleave them, so only the line
    # starting {"components": is compared; the host's whole output is kept in
    # logs/registry-host-<name>.log.
    registry() {
        local name=$1 rc=0; shift
        env -i HOME="$HOME" PATH=/usr/bin:/bin SDL_VIDEODRIVER=x11 \
            XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
            VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
            xvfb-run -a -s "-screen 0 1280x720x24" "$@" --print-registry 2>&1 \
            | tee "logs/registry-host-$name.log" | { grep '^{"components":' || true; } > "logs/registry-$name.json" || rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "::error::the $name host failed under --print-registry (exit $rc); its output follows (logs/registry-host-$name.log)"
            tail -n 60 "logs/registry-host-$name.log" || true
        fi
        return "$rc"
    }
    registry modular "$bindir/ArcaneRuntime/ArcaneRuntime" --project "$root/ReferenceProject" || exit 1
    registry package "$pkg/ReferenceProject.sh" || exit 1
    if [ ! -s logs/registry-modular.json ] && [ ! -s logs/registry-package.json ]; then
        echo "::error::neither host printed a registry line starting {\"components\": (logs/registry-host-*.log)"; exit 1
    fi
    if [ ! -s logs/registry-modular.json ]; then
        echo "::error::the modular runtime printed no registry line (logs/registry-host-modular.log)"; exit 1
    fi
    if [ ! -s logs/registry-package.json ]; then
        echo "::error::the packaged game printed no registry line (logs/registry-host-package.log)"; exit 1
    fi
    echo "registry (package): $(cat logs/registry-package.json)"
    if ! cmp -s logs/registry-modular.json logs/registry-package.json; then
        echo "::error::the packaged game's registry differs from the modular runtime's (logs/registry-*.json)"
        echo "registry (modular): $(cat logs/registry-modular.json)"
        diff logs/registry-modular.json logs/registry-package.json || true
        exit 1
    fi
    # Positive anchor: equality alone would pass a roster that lost the game's
    # entries on BOTH sides.
    for n in TintOnContact PlayerController2D; do
        if ! grep -q "$n" logs/registry-package.json; then
            echo "::error::the registry lacks the ReferenceGame entry $n (the game module's registrations did not survive)"; exit 1
        fi
    done
    echo "registrar survival: $(wc -c < logs/registry-package.json) bytes, identical, ReferenceGame roster present"
    readelf -d "$bindir/ArcaneServer/ArcaneServer" | { grep NEEDED || true; } > logs/server-imports.txt
else
    otool -L "$exe" | tee logs/package-imports.txt
    if grep -q libArcane logs/package-imports.txt; then
        echo "::error::the package imports an Arcane module"; exit 1
    fi
    env -i HOME="$HOME" PATH=/usr/bin:/bin "$pkg/ReferenceProject.sh" --print-engine-info \
        2>&1 | tee logs/package-boot.log
    otool -L "$bindir/ArcaneServer/ArcaneServer" > logs/server-imports.txt
fi
# Host isolation (modularization s6): the server imports no Client, Developer
# or Editor module. The forbidden list is each .arcmodule's "type" across
# Source/ AND ThirdParty/ (ImGui, SDL3 and ImGuiNodeEditor live in the latter),
# never folder names.
forbidden=$(for f in $(find Source ThirdParty -name '*.arcmodule' | sort); do
    awk -F'"' '/"name"/ && !n { n=$4 } /"type"/ && !t { t=$4 } END { if (t=="Client" || t=="Developer" || t=="Editor") print n }' "$f"
done)
if [ -z "$forbidden" ]; then
    echo "::error::host isolation: no Client/Developer/Editor module found under Source/ and ThirdParty/ (.arcmodule layout moved?)"; exit 1
fi
if [ ! -s logs/server-imports.txt ]; then
    echo "::error::host isolation: the ArcaneServer import dump is empty"; exit 1
fi
# Positive control: a known-good import must match the naming scheme used below.
if ! grep -q "libArcaneEngine\." logs/server-imports.txt; then
    echo "::error::host isolation: ArcaneServer's import dump does not show libArcaneEngine (naming scheme or dump broken)"; exit 1
fi
for m in $forbidden; do
    if grep -q "lib$m\." logs/server-imports.txt; then
        echo "::error::ArcaneServer imports $m (a Client/Developer/Editor module)"; exit 1
    fi
done
echo "host isolation: ArcaneServer imports no Client/Developer/Editor module ($(echo $forbidden | wc -w | tr -d ' ') checked: $(echo $forbidden))"
