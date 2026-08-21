#!/usr/bin/env bash
# Build fruitjam-coleco and leave the UF2 at build/fruitjam-coleco.uf2
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ -z "${PICO_SDK_PATH:-}" ]; then
    echo "error: PICO_SDK_PATH is not set." >&2
    echo "  git clone -b 2.1.1 https://github.com/raspberrypi/pico-sdk" >&2
    echo "  cd pico-sdk && git submodule update --init" >&2
    echo "  export PICO_SDK_PATH=\$PWD" >&2
    exit 1
fi

if [ ! -f "$ROOT/third_party/fatfs/ff.c" ]; then
    echo "Dependencies missing; running fetch_deps.sh..."
    "$ROOT/tools/fetch_deps.sh"
fi

cmake -S "$ROOT" -B "$ROOT/build" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPICO_BOARD=adafruit_fruit_jam \
      -DPICO_PLATFORM=rp2350-arm-s

cmake --build "$ROOT/build" --parallel "$(nproc 2>/dev/null || echo 4)"

echo
echo "Built: $ROOT/build/fruitjam-coleco.uf2"
ls -lh "$ROOT/build/fruitjam-coleco.uf2"
