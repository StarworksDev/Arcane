#!/usr/bin/env bash
# ci-image.sh <image-repo> <tag> -- the body of ci.yml's ci-image job: make
# sure <image-repo>:<tag> is published, then print its digest (the Linux legs
# run on image@digest, never on the tag).
#   published (imagetools inspect succeeds) -> reuse it, do not push
#   SAME_REPO=true and PUBLISH=true         -> build it into the local store
#       (ci/linux/Dockerfile, the scripts/ build context, --load), run
#       ci/linux/verify-image.sh in it, and only THEN push it: a failed verify
#       publishes nothing, so no later run can reuse an unverified tag
#   SAME_REPO=true and PUBLISH is not true  -> the same build and verify, do
#       not push, write digest= (empty) and verified=local, exit 0. A pull
#       request takes this path. An empty digest skips the Linux legs.
#   otherwise (a fork pull request, or SAME_REPO unset) -> fail, naming the tag:
#       a fork cannot publish. The remedy runs fork-authored Dockerfile/script
#       content in the one job holding packages: write, so a maintainer
#       REVIEWS the ci/linux/ and scripts/ diff first, then runs the workflow
#       from a branch of this repository (docs/ci.md, "Image").
# The digest is the SHA-256 of the registry's raw manifest bytes
# (`imagetools inspect --raw`): format-agnostic, unlike parsing buildx's
# pretty-printed JSON. Appends digest=<sha256:...> to $GITHUB_OUTPUT when set.
# PUBLISH is true only for a non-pull-request run of refs/heads/main.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
image="${1:?usage: ci-image.sh <image-repo> <tag>}"
tag="${2:?usage: ci-image.sh <image-repo> <tag>}"
ref="$image:$tag"

# reuse | push | local-verify | refuse
publish_decision() {
    if [ "${INSPECT_OK:-false}" = true ]; then echo reuse; return 0; fi
    if [ "${SAME_REPO:-false}" != true ]; then echo refuse; return 0; fi
    if [ "${PUBLISH:-false}" = true ]; then echo push; return 0; fi
    echo local-verify
}

build_and_verify() {
    docker buildx build --target build --provenance=false --load \
        --build-context scripts="$REPO_ROOT/scripts" \
        -f "$REPO_ROOT/ci/linux/Dockerfile" -t "$ref" "$REPO_ROOT/ci/linux"
    if ! docker run --rm -v "$REPO_ROOT/ci/linux:/v:ro" "$ref" bash /v/verify-image.sh; then
        echo "::error::$ref failed ci/linux/verify-image.sh -- not pushed" >&2
        exit 1
    fi
}

INSPECT_OK=false
if docker buildx imagetools inspect "$ref" > /dev/null 2>&1; then
    INSPECT_OK=true
fi

case "$(publish_decision)" in
    reuse)
        echo "ci-image: $ref is published"
        ;;
    push)
        echo "ci-image: $ref is not published -- building, verifying, then pushing it"
        build_and_verify
        docker push "$ref"
        ;;
    local-verify)
        echo "ci-image: $ref is not published -- building and verifying locally, not pushing"
        build_and_verify
        if [ -n "${GITHUB_OUTPUT:-}" ]; then
            echo "digest=" >> "$GITHUB_OUTPUT"
            echo "verified=local" >> "$GITHUB_OUTPUT"
        fi
        exit 0
        ;;
    refuse)
        echo "::error::$ref is not published, and a fork pull request cannot publish it. A maintainer reviews the ci/linux/ and scripts/ changes, then runs this workflow from a branch of this repository (docs/ci.md, Image)." >&2
        exit 1
        ;;
esac

raw=$(mktemp)
trap 'rm -f "$raw"' EXIT
docker buildx imagetools inspect --raw "$ref" > "$raw" || true
if [ ! -s "$raw" ]; then
    echo "::error::could not read the manifest of $ref" >&2
    exit 1
fi
if command -v sha256sum > /dev/null 2>&1; then sum=$(sha256sum "$raw"); else sum=$(shasum -a 256 "$raw"); fi
digest="sha256:${sum%% *}"
echo "ci-image: $ref = $digest"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "digest=$digest" >> "$GITHUB_OUTPUT"; fi
