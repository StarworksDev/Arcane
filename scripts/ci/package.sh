#!/usr/bin/env bash
# package.sh <Config> -- the monolithic ReferenceProject package (engine
# modularization s3.3 / s6), staged with `arcbuild package` and run IN PLACE
# from Packages/ with no SDK, no Binaries/ and a bare PATH. Linux (the CI image or a
# bare runner) and macOS; scripts/ci/package.ps1 is the Windows twin. Run after
# scripts/ci/build.sh <Config>. Logs: logs/package*.{log,txt}.
#   Linux: ldd shows no Arcane module and the package's OWN libSDL3 (RUNPATH
#          $ORIGIN), then a windowed boot under Xvfb + lavapipe (--frames 60).
#   macOS: otool -L shows no Arcane module, then --print-engine-info (the
#          hosted runner's windowed boot is not this lane's job).
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
    # Registrar survival (modularization s6): the package boots with the same
    # component and system roster as the modular runtime (--print-registry: one
    # sorted JSON line on stdout).
    registry() {
        env -i HOME="$HOME" PATH=/usr/bin:/bin SDL_VIDEODRIVER=x11 \
            XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
            VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
            xvfb-run -a -s "-screen 0 1280x720x24" "$@" --print-registry 2>>logs/registry.err
    }
    registry "$bindir/ArcaneRuntime/ArcaneRuntime" --project "$root/ReferenceProject" > logs/registry-modular.json
    registry "$pkg/ReferenceProject.sh" > logs/registry-package.json
    if [ ! -s logs/registry-modular.json ] || ! cmp -s logs/registry-modular.json logs/registry-package.json; then
        echo "::error::the packaged game's registry differs from the modular runtime's (logs/registry-*.json)"
        diff logs/registry-modular.json logs/registry-package.json || true
        exit 1
    fi
    echo "registrar survival: $(wc -c < logs/registry-package.json) bytes, identical"
    readelf -d "$bindir/ArcaneServer/ArcaneServer" | grep NEEDED > logs/server-imports.txt
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
# or Editor module -- the module list is read from Source/, not kept here.
for m in $(ls Source/Client Source/Developer Source/Editor); do
    if grep -q "lib$m\." logs/server-imports.txt; then
        echo "::error::ArcaneServer imports $m (a Client/Developer/Editor module)"; exit 1
    fi
done
echo "host isolation: ArcaneServer imports no Client/Developer/Editor module"
