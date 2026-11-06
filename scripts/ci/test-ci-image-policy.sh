#!/usr/bin/env bash
# publish_decision in scripts/ci/ci-image.sh, extracted and eval'd. No docker.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
extract=$(sed -n '/^publish_decision()/,/^}/p' "$ROOT/scripts/ci/ci-image.sh")
if [ -z "$extract" ]; then
    echo "publish_decision missing"
    exit 1
fi
eval "$extract"

INSPECT_OK=true SAME_REPO=true PUBLISH=true
test "$(publish_decision)" = reuse
INSPECT_OK=false SAME_REPO=true PUBLISH=true
test "$(publish_decision)" = push
INSPECT_OK=false SAME_REPO=true PUBLISH=false
test "$(publish_decision)" = local-verify
INSPECT_OK=false SAME_REPO=false PUBLISH=true
test "$(publish_decision)" = refuse

echo "test-ci-image-policy: all passed"
