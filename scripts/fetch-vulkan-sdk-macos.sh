#!/bin/sh
# fetch-vulkan-sdk-macos.sh -- installs the pinned LunarG Vulkan SDK for macOS:
#   * MoltenVK (Apache-2.0), the Vulkan-on-Metal ICD Arcane's EXISTING Vulkan
#     backend runs on (Arcane has no Metal backend -- D3D12 + Vulkan only);
#   * the Vulkan loader, libvulkan.1.dylib, which NRI and vulkan.hpp dlopen
#     by leaf name, and VK_LAYER_KHRONOS_validation;
#   * DXC (bin/dxc, lib/libdxcompiler.dylib, include/dxc/dxcapi.h): the build-
#     time shader compiler and the runtime compile service. Microsoft publishes
#     no macOS DXC release, so the SDK is the one pinned, checksummed source --
#     chosen over cross-compiling shaders on another leg (a cross-job artifact
#     the macOS leg could not rebuild on its own) and over building DXC from
#     source (an hour of LLVM per cache miss).
#
#   scripts/fetch-vulkan-sdk-macos.sh [--system]
#     Installs to ThirdParty/tools/vulkan-sdk-macos/<version> (gitignored) and
#     links DXC into ThirdParty/tools/dxc-macos/ (bin/dxc is a wrapper script,
#     lib/ and include/ are symlinks), where compile-shaders.sh, the
#     ArcaneClient include path and the host postbuilds look.
#     --system also performs the SDK's "system global installation" (sudo):
#     /usr/local/lib/libvulkan.1.dylib + /usr/local/share/vulkan/{icd.d,
#     explicit_layer.d}, i.e. on dyld's default fallback path, which is how a
#     leaf-name dlopen finds the loader. CI passes it; a developer who ran the
#     SDK's own installer already has that.
#
# Pinned: version + SHA-256 of the SDK zip (bump together; docs/ci.md).
# 1.4.321.0 deliberately predates the SDK's KosmicKrisp driver: MoltenVK is the
# only ICD it installs, so "Vulkan on a Mac" means exactly one thing.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VK_SDK_PINNED_VERSION=1.4.321.0
VK_SDK_PINNED_SHA256=d873c43acacec1e3330fb530dafd541aa5d8a5726575a98a3f70ca505fc203db
VK_SDK_VERSION="${VK_SDK_VERSION:-$VK_SDK_PINNED_VERSION}"
if [ -z "${VK_SDK_SHA256:-}" ]; then
    if [ "$VK_SDK_VERSION" != "$VK_SDK_PINNED_VERSION" ]; then
        echo "fetch-vulkan-sdk-macos: $VK_SDK_VERSION is not the pinned $VK_SDK_PINNED_VERSION; set VK_SDK_SHA256" >&2
        exit 1
    fi
    VK_SDK_SHA256="$VK_SDK_PINNED_SHA256"
fi
SYSTEM=0
[ "${1:-}" = "--system" ] && SYSTEM=1

[ "$(uname -s)" = Darwin ] || { echo "fetch-vulkan-sdk-macos: macOS only" >&2; exit 1; }

DEST="$ROOT/ThirdParty/tools/vulkan-sdk-macos/$VK_SDK_VERSION"
CACHE="${VK_SDK_CACHE:-$ROOT/ThirdParty/tools/vulkan-sdk-macos/download}"
ZIP="$CACHE/vulkansdk-macos-$VK_SDK_VERSION.zip"
URL="https://sdk.lunarg.com/sdk/download/$VK_SDK_VERSION/mac/vulkansdk-macos-$VK_SDK_VERSION.zip"

mkdir -p "$CACHE"
if [ ! -f "$ZIP" ]; then
    echo "fetch-vulkan-sdk-macos: $URL"
    curl -fsSL --retry 4 -o "$ZIP.part" "$URL"
    mv "$ZIP.part" "$ZIP"
fi
# Verified every run, cached or not. VK_SDK_BOOTSTRAP=1 is the one-time pin
# procedure (print the digest, continue) for a version not pinned yet.
if [ "${VK_SDK_BOOTSTRAP:-0}" = 1 ] && [ "$VK_SDK_SHA256" = 0000000000000000000000000000000000000000000000000000000000000000 ]; then
    echo "::warning::fetch-vulkan-sdk-macos: UNPINNED bootstrap -- $(shasum -a 256 "$ZIP")"
else
    "$ROOT/scripts/sha256-check.sh" "$ZIP" "$VK_SDK_SHA256"
fi

if [ ! -x "$DEST/macOS/bin/dxc" ] || [ "$SYSTEM" = 1 ]; then
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    ditto -x -k "$ZIP" "$TMP"
    app=$(find "$TMP" -maxdepth 2 -name '*.app' -type d | head -n 1)
    [ -n "$app" ] || { echo "fetch-vulkan-sdk-macos: no installer .app in $ZIP" >&2; exit 1; }
    installer="$app/Contents/MacOS/$(basename "$app" .app)"
    components="com.lunarg.vulkan.core"
    [ "$SYSTEM" = 1 ] && components="$components com.lunarg.vulkan.usr"
    rm -rf "$DEST"
    mkdir -p "$(dirname "$DEST")"
    echo "fetch-vulkan-sdk-macos: installing $components to $DEST"
    if [ "$SYSTEM" = 1 ]; then
        # shellcheck disable=SC2086 -- $components is a word list.
        sudo "$installer" --root "$DEST" --accept-licenses --default-answer --confirm-command install $components
        sudo chown -R "$(id -u):$(id -g)" "$DEST"
    else
        # shellcheck disable=SC2086
        "$installer" --root "$DEST" --accept-licenses --default-answer --confirm-command install $components
    fi
fi

SDK="$DEST/macOS"
for f in bin/dxc lib/libdxcompiler.dylib include/dxc/dxcapi.h lib/libvulkan.1.dylib lib/libMoltenVK.dylib; do
    [ -e "$SDK/$f" ] || { echo "fetch-vulkan-sdk-macos: $SDK/$f missing from the SDK" >&2; ls -R "$DEST" | head -100 >&2; exit 1; }
done

DXC_DIR="$ROOT/ThirdParty/tools/dxc-macos"
rm -rf "$DXC_DIR"
mkdir -p "$DXC_DIR/bin"
ln -s "$SDK/lib" "$DXC_DIR/lib"
ln -s "$SDK/include" "$DXC_DIR/include"
# A wrapper, not a symlink: the SDK's dxc finds libdxcompiler.dylib through
# its own @executable_path-relative rpath.
cat > "$DXC_DIR/bin/dxc" <<WRAP
#!/bin/sh
exec "$SDK/bin/dxc" "\$@"
WRAP
chmod +x "$DXC_DIR/bin/dxc"
"$DXC_DIR/bin/dxc" --version || true

echo "fetch-vulkan-sdk-macos: VULKAN_SDK=$SDK"
if [ "$SYSTEM" = 1 ]; then
    ls -l /usr/local/lib/libvulkan* /usr/local/share/vulkan/icd.d/ || true
fi
