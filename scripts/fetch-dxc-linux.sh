#!/bin/sh
# Fetches the Linux DirectX Shader Compiler release into
# ThirdParty/tools/dxc-linux/ (gitignored) -- the Linux counterpart of the
# vendored Windows drop in ThirdParty/tools/dxc/. Two consumers:
#   * data/shaders/compile-shaders.sh (the ArcaneClient prebuild) runs bin/dxc;
#   * the runtime compile service (ShaderCompiler) dlopen()s lib/libdxcompiler.so,
#     which the host/test postbuilds stage beside each exe when present.
# Not vendored: the Linux release is ~32 MB of binaries, and the Windows drop
# (v1.9.2602.24) has no Linux asset name this script could pin blind. Override
# the release with DXC_TAG/DXC_ASSET (and then DXC_SHA256 for that asset).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/ThirdParty/tools/dxc-linux"
# The pinned release, asset and the asset's SHA-256 (bump together).
DXC_PINNED_TAG=v1.8.2505.1
DXC_PINNED_ASSET=linux_dxc_2025_07_14.x86_64.tar.gz
DXC_PINNED_SHA256=f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c
DXC_TAG="${DXC_TAG:-$DXC_PINNED_TAG}"
DXC_ASSET="${DXC_ASSET:-$DXC_PINNED_ASSET}"
if [ -z "${DXC_SHA256:-}" ]; then
    if [ "$DXC_TAG" != "$DXC_PINNED_TAG" ] || [ "$DXC_ASSET" != "$DXC_PINNED_ASSET" ]; then
        echo "fetch-dxc-linux: $DXC_TAG/$DXC_ASSET is not the pinned $DXC_PINNED_TAG/$DXC_PINNED_ASSET; set DXC_SHA256 to its SHA-256" >&2
        exit 1
    fi
    DXC_SHA256="$DXC_PINNED_SHA256"
fi
URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DXC_TAG/$DXC_ASSET"

if [ -x "$DEST/bin/dxc" ] && [ -f "$DEST/lib/libdxcompiler.so" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "fetch-dxc-linux: already present at $DEST ($("$DEST/bin/dxc" --version 2>/dev/null || echo unknown))"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "fetch-dxc-linux: $URL"
curl -fsSL --retry 4 -o "$TMP/dxc.tar.gz" "$URL"
if ! "$ROOT/scripts/sha256-check.sh" "$TMP/dxc.tar.gz" "$DXC_SHA256"; then
    echo "fetch-dxc-linux: SHA-256 MISMATCH for $URL (expected $DXC_SHA256) -- refusing to install" >&2
    exit 1
fi
rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$TMP/dxc.tar.gz" -C "$DEST"
chmod +x "$DEST/bin/dxc"
"$DEST/bin/dxc" --version
