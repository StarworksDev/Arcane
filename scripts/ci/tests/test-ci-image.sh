#!/usr/bin/env bash
# Tests for scripts/ci/ci-image.sh with a stub `docker` on PATH (no daemon,
# no registry): a published tag is reused, a missing one is built, verified
# and only THEN pushed on a same-repo run (a failed verify publishes nothing),
# and refused -- never built -- on a fork PR. A same-repo run pushes only when
# PUBLISH=true; otherwise it verifies locally and writes an empty digest.
# The stub's `imagetools inspect
# --raw` prints manifest bytes the way buildx does; the digest is their SHA-256.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fails=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1" >&2; fails=$((fails + 1)); }

# STATE dir: calls.txt records every docker call; "built" exists once an image
# is in the local store, "published" once it is in the (fake) registry.
# manifest.json holds the raw manifest the registry serves (pretty-printed,
# like real buildx output). BAD_DIGEST=1 makes `inspect --raw` print nothing;
# FAIL_VERIFY=1 makes `docker run` (verify-image.sh) fail.
mkstub() {
    local d=$1
    mkdir -p "$d/bin"
    printf '{\n  "schemaVersion": 2,\n  "mediaType": "application/vnd.oci.image.manifest.v1+json",\n  "config": {\n    "digest": "sha256:%064d",\n    "size": 1\n  }\n}\n' 0 > "$d/manifest.json"
    cat > "$d/bin/docker" <<EOF
#!/bin/sh
echo "docker \$*" >> "$d/calls.txt"
case "\$*" in
  "buildx imagetools inspect"*)
    [ -f "$d/published" ] || { echo "ERROR: ghcr.io/x: not found" >&2; exit 1; }
    case "\$*" in *--raw*) [ -n "\${BAD_DIGEST:-}" ] || cat "$d/manifest.json" ;; esac
    exit 0 ;;
  "buildx build"*) touch "$d/built"; case "\$*" in *--push*) touch "$d/published" ;; esac; exit 0 ;;
  "push "*) [ -f "$d/built" ] || exit 1; touch "$d/published"; exit 0 ;;
  "run "*) [ -n "\${FAIL_VERIFY:-}" ] && exit 1; exit 0 ;;
esac
exit 0
EOF
    chmod +x "$d/bin/docker"
}
digest_of() { echo "sha256:$(sha256sum "$1/manifest.json" | cut -d' ' -f1)"; }
run() {   # run <state> [VAR=value...]
    local d=$1; shift
    : > "$d/calls.txt"; : > "$d/out.txt"
    set +e
    env -u SAME_REPO -u PUBLISH -u GITHUB_OUTPUT PATH="$d/bin:$PATH" GITHUB_OUTPUT="$d/gh_output" "$@" \
        "$REPO/scripts/ci/ci-image.sh" ghcr.io/starworksdev/arcane-ci build-0123456789abcdef > "$d/out.txt" 2>&1
    RC=$?
    set -e
}
line_of() { grep -n -- "$2" "$1/calls.txt" | head -n 1 | cut -d: -f1 || true; }

d=$(mktemp -d); mkstub "$d"; touch "$d/published"; run "$d" SAME_REPO=true
[ "$RC" -eq 0 ] && ! grep -q "buildx build" "$d/calls.txt" && grep -qx "digest=$(digest_of "$d")" "$d/gh_output" \
    && ok "published tag reused, digest = sha256 of the raw manifest" || bad "published tag reused (rc $RC): $(cat "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=true PUBLISH=true
v=$(line_of "$d" "verify-image.sh"); p=$(line_of "$d" "^docker push ghcr.io/starworksdev/arcane-ci:build-0123456789abcdef$")
grep -q "buildx build --target build --provenance=false --load" "$d/calls.txt" \
    && ! grep -q -- "--push" "$d/calls.txt" \
    && grep -q -- "--build-context scripts=$REPO/scripts" "$d/calls.txt" \
    && [ -n "$v" ] && [ -n "$p" ] && [ "$v" -lt "$p" ] \
    && [ "$RC" -eq 0 ] && grep -qx "digest=$(digest_of "$d")" "$d/gh_output" \
    && ok "missing tag built, verified, THEN pushed" || bad "missing tag built (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=true PUBLISH=true FAIL_VERIFY=1
[ "$RC" -eq 1 ] && ! grep -q "^docker push" "$d/calls.txt" && [ ! -f "$d/published" ] && [ ! -s "$d/gh_output" ] \
    && ok "a failed verify publishes nothing" || bad "a failed verify publishes nothing (rc $RC): $(cat "$d/calls.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=true PUBLISH=false
grep -q "buildx build --target build --provenance=false --load" "$d/calls.txt" \
    && ! grep -q "^docker push" "$d/calls.txt" && ! grep -q -- "--push" "$d/calls.txt" \
    && [ ! -f "$d/published" ] && [ "$RC" -eq 0 ] \
    && grep -qx "digest=" "$d/gh_output" && grep -qx "verified=local" "$d/gh_output" \
    && ok "missing tag on a non-publish run is verified locally and not pushed" \
    || bad "local verify (rc $RC): $(cat "$d/calls.txt" "$d/out.txt" "$d/gh_output")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=false PUBLISH=true
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" \
    && ok "PUBLISH does not let a fork build" || bad "fork with PUBLISH (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d" SAME_REPO=false
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" && grep -q "build-0123456789abcdef" "$d/out.txt" \
    && ok "fork PR refused, names the tag, builds nothing" || bad "fork PR refused (rc $RC): $(cat "$d/calls.txt" "$d/out.txt")"

d=$(mktemp -d); mkstub "$d"; run "$d"
[ "$RC" -eq 1 ] && ! grep -q "buildx build" "$d/calls.txt" && ok "unset SAME_REPO counts as a fork" || bad "unset SAME_REPO counts as a fork (rc $RC)"

d=$(mktemp -d); mkstub "$d"; touch "$d/published"; run "$d" SAME_REPO=true BAD_DIGEST=1
[ "$RC" -eq 1 ] && [ ! -s "$d/gh_output" ] && ok "unreadable digest fails" || bad "unreadable digest fails (rc $RC)"

echo
[ "$fails" -eq 0 ] && echo "test-ci-image: all passed" || echo "test-ci-image: $fails failure(s)"
[ "$fails" -eq 0 ]
