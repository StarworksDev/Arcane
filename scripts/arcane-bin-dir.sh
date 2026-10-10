#!/bin/sh
# Prints the repo-relative bin flavor directory premake5.lua's outputdir
# produces for <Config> on THIS host: bin/<Config>-<system>-<arch>-md.
#   Linux:  bin/<Config>-linux-x86_64-md
#   macOS:  bin/<Config>-macosx-AARCH64-md (Apple Silicon; premake beta8 spells the
#           arm64 architecture "AARCH64") or -macosx-x86_64-md on an Intel Mac.
# ARCANE_MAC_ARCH (arm64|x86_64) overrides the Mac architecture, exactly as it
# does in premake5.lua. Sourced or run: `scripts/arcane-bin-dir.sh Debug`.
set -eu
config="${1:-Debug}"
case "$(uname -s)" in
    Linux)  system=linux;  arch=x86_64 ;;
    Darwin)
        system=macosx
        want="${ARCANE_MAC_ARCH:-$(uname -m)}"
        case "$want" in
            arm64|aarch64|ARM64) arch=AARCH64 ;;
            x86_64|x64)          arch=x86_64 ;;
            *) echo "arcane-bin-dir: ARCANE_MAC_ARCH must be arm64 or x86_64, got '$want'" >&2; exit 2 ;;
        esac ;;
    *) echo "arcane-bin-dir: unsupported host $(uname -s)" >&2; exit 2 ;;
esac
echo "bin/$config-$system-$arch-md"
