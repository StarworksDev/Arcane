#!/bin/sh
# Fetches the premake5 release for THIS host (Linux or macOS) into
# ThirdParty/premake5/premake5 (gitignored) -- the POSIX twin of the vendored
# ThirdParty/premake5/premake5.exe. Arcane::Toolchain::ResolvePremake looks for
# exactly this file on POSIX (bundled copy first, then PATH), so arcbuild, the
# editor's module build and the [build-generator] tests find the same premake
# a Windows checkout does. Same version as the vendored .exe (5.0.0-beta8).
#
# Pinned: the version and the SHA-256 of each host's release tarball (bump
# together). Overriding PREMAKE_VERSION requires PREMAKE_SHA256 too -- a
# download is never trusted on transport alone (docs/ci.md, "Downloads").
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/ThirdParty/premake5/premake5"
PREMAKE_PINNED_VERSION=5.0.0-beta8
case "$(uname -s)" in
    Linux)
        ASSET_OS=linux
        PREMAKE_PINNED_SHA256=63edd3e7461eebdd45b500a3c7e8ad4e7a67d68f230010f9a97cbb71b4ec59c8 ;;
    Darwin)
        # The beta8 macOS binary is arm64-only (Mach-O arm64); an Intel Mac
        # runs it under Rosetta 2.
        ASSET_OS=macosx
        PREMAKE_PINNED_SHA256=fa73a46f093fa6f17494a3d063421aa6cae3ea825a61c62dd59fc2f07a256d03 ;;
    *) echo "fetch-premake: unsupported host $(uname -s)" >&2; exit 1 ;;
esac
PREMAKE_VERSION="${PREMAKE_VERSION:-$PREMAKE_PINNED_VERSION}"
if [ -z "${PREMAKE_SHA256:-}" ]; then
    if [ "$PREMAKE_VERSION" != "$PREMAKE_PINNED_VERSION" ]; then
        echo "fetch-premake: PREMAKE_VERSION=$PREMAKE_VERSION is not the pinned $PREMAKE_PINNED_VERSION; set PREMAKE_SHA256 to its tarball's SHA-256" >&2
        exit 1
    fi
    PREMAKE_SHA256="$PREMAKE_PINNED_SHA256"
fi
URL="https://github.com/premake/premake-core/releases/download/v$PREMAKE_VERSION/premake-$PREMAKE_VERSION-$ASSET_OS.tar.gz"

if [ -x "$DEST" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "fetch-premake: already present ($("$DEST" --version))"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "fetch-premake: $URL"
curl -fsSL --retry 4 -o "$TMP/premake.tar.gz" "$URL"
"$ROOT/scripts/sha256-check.sh" "$TMP/premake.tar.gz" "$PREMAKE_SHA256"
tar -xzf "$TMP/premake.tar.gz" -C "$TMP"
mkdir -p "$(dirname "$DEST")"
cp -f "$TMP/premake5" "$DEST"
chmod 0755 "$DEST"
"$DEST" --version
