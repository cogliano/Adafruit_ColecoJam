#!/usr/bin/env bash
# Fetch the two third-party sources this project needs.
# Run once after cloning.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

# --- FatFs (ELM-ChaN) -----------------------------------------------------
if [ ! -f "$TP/fatfs/ff.c" ]; then
    echo "Fetching FatFs R0.15..."
    tmp="$(mktemp -d)"
    curl -fsSL http://elm-chan.org/fsw/ff/arc/ff15.zip -o "$tmp/ff.zip"
    unzip -q "$tmp/ff.zip" -d "$tmp"
    mkdir -p "$TP/fatfs"
    cp "$tmp"/source/*.c "$tmp"/source/*.h "$TP/fatfs/"
    rm -rf "$tmp"
    # Install our tuned ffconf.h over the stock one.
    cp "$ROOT/tools/ffconf.h" "$TP/fatfs/ffconf.h"
else
    echo "FatFs already present."
fi

# --- Pico-PIO-USB ---------------------------------------------------------
if [ ! -f "$TP/Pico-PIO-USB/src/pio_usb.c" ]; then
    echo "Fetching Pico-PIO-USB..."
    # Pin to 0.7.2. Working Fruit Jam projects (retroJam, pico-snesPlus) call
    # out this release specifically; tracking master risks an untested change.
    git clone --depth 1 --branch 0.7.2 \
        https://github.com/sekigon-gonnoc/Pico-PIO-USB.git \
        "$TP/Pico-PIO-USB"
else
    echo "Pico-PIO-USB already present."
fi

echo
echo "Done. Now:"
echo "  export PICO_SDK_PATH=/path/to/pico-sdk"
echo "  ./tools/build.sh"
