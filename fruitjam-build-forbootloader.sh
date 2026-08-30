#!/usr/bin/env bash
# Build Adafruit_ColecoJam for the resident pico-bootLoader.
#
# Differences from tools/build.sh:
#   * -DBUILD_FOR_BOOTLOADER=ON, which relinks FLASH to ORIGIN 0x10080000
#     (see cmake/BootPartition.cmake for the map).
#   * a separate build tree, so the two variants never share stale objects.
#
# Output: build_bl_fruitjam/colecojam.uf2
#
# Do NOT drag that file onto the Fruit Jam over USB -- it is linked for the
# application partition and will not boot on its own. Put it on the
# pico-bootLoader SD card as /emu/8/colecojam.uf2 and let the picker flash it.
#
# The name and layout of this script are load-bearing: pico-bootLoader's
# build_emulators.sh derives them from its SCRIPTED_* tables as
# <board>-build-forbootloader.sh, build_bl_<board>/, and an artifact named
# exactly <program_name>.uf2. Renaming any of the three breaks that build.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
TAG=fruitjam
BUILD="$ROOT/build_bl_${TAG}"

if [ -z "${PICO_SDK_PATH:-}" ] && [ ! -f "$HOME/.pico-sdk/cmake/pico-vscode.cmake" ]; then
    echo "error: PICO_SDK_PATH is not set and no VS Code SDK was found." >&2
    echo "  git clone -b 2.3.0 https://github.com/raspberrypi/pico-sdk" >&2
    echo "  cd pico-sdk && git submodule update --init" >&2
    echo "  export PICO_SDK_PATH=\$PWD" >&2
    exit 1
fi

# build_emulators.sh builds from a fresh shallow clone, which has no
# third_party/ at all. fetch_deps.sh is idempotent and covers all three
# dependencies, so just run it every time. Invoked through bash because the
# scripts under tools/ are not checked in with the executable bit set.
bash "$ROOT/tools/fetch_deps.sh"

cmake -S "$ROOT" -B "$BUILD" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPICO_BOARD=adafruit_fruit_jam \
      -DPICO_PLATFORM=rp2350-arm-s \
      -DBUILD_FOR_BOOTLOADER=ON

cmake --build "$BUILD" --parallel "$(nproc 2>/dev/null || echo 4)"

echo
echo "Built: $BUILD/colecojam.uf2"
ls -lh "$BUILD/colecojam.uf2"
