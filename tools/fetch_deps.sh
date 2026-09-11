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

# --- pico_hdmi -------------------------------------------------------------
# Needed only for VIDEO_DRIVER_PICO_HDMI, which is what carries audio over HDMI.
if [ ! -f "$TP/pico_hdmi/CMakeLists.txt" ]; then
    echo "Fetching pico_hdmi..."
    git clone --depth 1 https://github.com/fliperama86/pico_hdmi.git \
        "$TP/pico_hdmi"
else
    echo "pico_hdmi already present."
fi

# --- tusb_xinput -----------------------------------------------------------
# TinyUSB host class driver for XInput pads (Xbox 360 / One / Series).
#
# Pinned to cfd83ba, NOT the fork's tip. The very next commit (ae4f3a9, "Fix
# return type and value for xinputh_open to conform to new API") changes
# xinputh_open() from bool to uint16_t for a TinyUSB revision newer than the
# one Pico SDK 2.3.0 ships, whose usbh_class_driver_t still declares
#
#     bool (* const open)(uint8_t, uint8_t, tusb_desc_interface_t const *, uint16_t);
#
# Building the tip against 2.3.0 fails outright on that incompatible pointer
# type. If this project is ever moved to an SDK carrying the newer TinyUSB,
# move this pin forward to 915b21ac at the same time.
#
# A full clone is needed because --depth 1 cannot reach an arbitrary SHA.
if [ ! -f "$TP/tusb_xinput/xinput_host.c" ]; then
    echo "Fetching tusb_xinput..."
    git clone https://github.com/PicoPlus-devel/tusb_xinput.git "$TP/tusb_xinput"
    git -C "$TP/tusb_xinput" checkout --quiet \
        cfd83ba9b0809cf69f7b63d351f44ff73ebd0e30
else
    echo "tusb_xinput already present."
fi

echo
echo "Done. Now:"
echo "  export PICO_SDK_PATH=/path/to/pico-sdk"
echo "  ./tools/build.sh"
