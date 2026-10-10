#!/usr/bin/env bash
# Kept for existing docs and muscle memory: scripts/run-tests.sh is the one
# Linux/macOS test entry point now (macOS port, 2026-10-07) -- same arguments
# ([--gpu] <Config> [--rng-seed N] [Catch2 args...]), same exclusion list
# (scripts/linux-test-exclusions.txt on Linux).
exec "$(dirname "$0")/run-tests.sh" "$@"
