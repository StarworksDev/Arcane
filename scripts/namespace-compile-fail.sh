#!/usr/bin/env bash
# Flat gameplay API: the control must compile; every forbidden spelling must fail.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CXX:-c++}
cases="$root/ArcaneTests/compile-fail/namespace"
log=$(mktemp)
trap 'rm -f -- "$log"' EXIT

includes=(
  -I"$root/ArcaneCore/src"
  -I"$root/ThirdParty/nlohmann"
  -I"$root/ThirdParty/picosha2"
  -I"$root/ThirdParty/spdlog/include"
  -I"$root/ThirdParty/glm"
  -I"$root/ThirdParty/stb"
  -I"$root/ThirdParty/Astra/include"
  -I"$root/ThirdParty/enkiTS/src"
  -I"$root/ThirdParty/Manifold2D/include"
  -I"$root/ThirdParty/Mosaic/include"
)

if ! "$compiler" -std=c++23 -fsyntax-only "${includes[@]}" "$cases/Control.cpp" >"$log" 2>&1; then
  cat "$log"
  echo 'namespace-compile-fail: the CONTROL failed to compile'
  exit 1
fi

for name in EcsEntity Physics2DWorld ArcanePhys PhysicsResource PhysicsSystem \
            RigidBody PhysicsWorld Body BodyHandle WorldMember; do
  if "$compiler" -std=c++23 -fsyntax-only "${includes[@]}" "$cases/$name.cpp" >"$log" 2>&1; then
    echo "namespace-compile-fail: $name.cpp compiled; that spelling must not exist"
    exit 1
  fi
done

echo 'namespace-compile-fail: PASS (control compiles; 10 forbidden spellings do not)'
