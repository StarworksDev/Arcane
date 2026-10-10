#!/bin/sh
# build-sdl3.sh <install-prefix> -- builds SDL3 from its pinned release
# tarball into <install-prefix> (shared library, no tests/examples). The one
# SDL3 recipe for Linux and macOS: neither Ubuntu 24.04 nor a stock Mac ships
# SDL3, and Windows takes it from vcpkg (scripts/setup-vcpkg-deps.bat).
#
# Pinned: SDL3_VERSION + the SHA-256 of its release tarball (bump together;
# docs/ci.md, "Downloads"). Override both with SDL3_VERSION/SDL3_SHA256.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="${1:?usage: build-sdl3.sh <install-prefix>}"
SDL3_PINNED_VERSION=3.2.22
SDL3_PINNED_SHA256=f29d00cbcee273c0a54f3f32f86bf5c595e8823a96b1d92a145aac40571ebfcc
SDL3_VERSION="${SDL3_VERSION:-$SDL3_PINNED_VERSION}"
if [ -z "${SDL3_SHA256:-}" ]; then
    if [ "$SDL3_VERSION" != "$SDL3_PINNED_VERSION" ]; then
        echo "build-sdl3: SDL3_VERSION=$SDL3_VERSION is not the pinned $SDL3_PINNED_VERSION; set SDL3_SHA256" >&2
        exit 1
    fi
    SDL3_SHA256="$SDL3_PINNED_SHA256"
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VERSION/SDL3-$SDL3_VERSION.tar.gz"
echo "build-sdl3: $URL"
curl -fsSL --retry 4 -o "$TMP/sdl3.tar.gz" "$URL"
"$ROOT/scripts/sha256-check.sh" "$TMP/sdl3.tar.gz" "$SDL3_SHA256"
tar -xzf "$TMP/sdl3.tar.gz" -C "$TMP"

generator="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && generator=Ninja
extra=""
if [ "$(uname -s)" = Darwin ]; then
    # The host architecture only (what premake5.lua builds the engine for).
    extra="-DCMAKE_OSX_ARCHITECTURES=$(uname -m)"
fi
# shellcheck disable=SC2086 -- $extra is empty or one word.
cmake -S "$TMP/SDL3-$SDL3_VERSION" -B "$TMP/build" -G "$generator" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF $extra
cmake --build "$TMP/build" --parallel
cmake --install "$TMP/build"
echo "build-sdl3: SDL3 $SDL3_VERSION installed to $PREFIX"
