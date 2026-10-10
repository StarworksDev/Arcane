#!/bin/sh
# Kept for existing docs and muscle memory: scripts/fetch-premake.sh is the
# one premake fetcher for Linux and macOS now (macOS port, 2026-10-07).
exec "$(dirname "$0")/fetch-premake.sh" "$@"
