#!/usr/bin/env bash
# Builds VK_LAYER_KHRONOS_validation from source into <prefix> (default
# /usr/local), for the Linux [gpu] lane and windowed smoke.
#
# WHY NOT THE DISTRO PACKAGE: Ubuntu 24.04's vulkan-validationlayers is
# 1.3.275, whose synchronization validation does not track HOST-side waits on
# timeline semaphores (neither vkWaitSemaphores nor vkGetSemaphoreCounterValue
# -- layers/sync/ has no record hook for either). Arcane paces frame slots by
# polling a timeline fence's counter (NriGraphContext.cpp /
# NriSwapChain.cpp PollingWaitForTimelineFence), so 1.3.275 reports every
# per-slot buffer rewrite two frames later as SYNC-HAZARD-WRITE-AFTER-WRITE --
# a false positive that fails every [gpu] case asserting RenderErrorCount() ==
# 0. The pinned tag below records both hooks (sync_validation.cpp
# PostCallRecordWaitSemaphores / PostCallRecordGetSemaphoreCounterValue) and is
# clean on the same runs.
#
#   scripts/build-vvl-linux.sh [prefix]     (VVL_TAG + VVL_COMMIT override the pin)
#
# The tag is fetched, then its commit is checked against VVL_COMMIT: a moved
# tag fails the build instead of silently changing the layer (roadmap D26).
#
# Needs cmake, ninja, python3, git and a C++ compiler; UPDATE_DEPS fetches
# Vulkan-Headers, SPIRV-Headers/Tools, Vulkan-Utility-Libraries and friends at
# the versions the tag's known_good.json names. Takes ~15 min on 3 cores.
#
# INSTALL INTO A LOADER SEARCH ROOT: the manifest names a bare
# "libVkLayer_khronos_validation.so", which dlopen resolves through the
# library path -- a distro copy of the same name earlier on that path wins
# silently. /usr/local (with the distro package NOT installed, then ldconfig)
# is the unambiguous choice.
set -euo pipefail

PREFIX="${1:-/usr/local}"
TAG="${VVL_TAG:-vulkan-sdk-1.4.328.0}"
COMMIT="${VVL_COMMIT:-a1ff2dbc7e50828def8098c5ebf0fee10b714f85}"
WORK="${VVL_WORK:-$(mktemp -d)}"
JOBS="${MAKE_JOBS:-3}"

git clone --depth 1 -b "$TAG" https://github.com/KhronosGroup/Vulkan-ValidationLayers "$WORK/vvl"
got=$(git -C "$WORK/vvl" rev-parse HEAD)
if [ "$got" != "$COMMIT" ]; then
    echo "build-vvl-linux: $TAG resolved to $got, expected $COMMIT -- refusing" >&2
    exit 1
fi
cmake -S "$WORK/vvl" -B "$WORK/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DUPDATE_DEPS=ON -DBUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$WORK/build" -j "$JOBS"
cmake --install "$WORK/build"
echo "build-vvl-linux: $TAG installed under $PREFIX"
