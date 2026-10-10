#!/bin/sh
# sha256-check.sh <file> <expected-sha256> -- exits non-zero (and prints the
# actual digest) unless <file> hashes to <expected>. sha256sum on Linux,
# shasum -a 256 on macOS (which has no sha256sum before macOS 15's coreutils).
set -eu
file="$1"; expected="$2"
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$file" | cut -d' ' -f1)
else
    actual=$(shasum -a 256 "$file" | cut -d' ' -f1)
fi
if [ "$actual" != "$expected" ]; then
    echo "sha256-check: SHA-256 MISMATCH for $file" >&2
    echo "  expected $expected" >&2
    echo "  actual   $actual" >&2
    echo "  -- refusing to use it" >&2
    exit 1
fi
echo "sha256-check: $(basename "$file") OK ($actual)"
