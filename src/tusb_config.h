// tusb_config.h -- TinyUSB configuration
//
// The Fruit Jam runs TinyUSB in both roles at once:
//   - Device on the native RP2350 USB controller (port 0, the USB-C jack):
//     mass storage, so ROMs can be dropped onto the SD card from a PC.
//   - Host on PIO-USB (port 1, GPIO1/GPIO2 feeding the CH334F hub that drives
//     the two USB-A jacks): HID gamepads.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Board / port assignment
// ---------------------------------------------------------------------------
// The Pico SDK already passes CFG_TUSB_MCU, CFG_TUSB_OS and CFG_TUSB_DEBUG on
// the compiler command line. Defining them again here is a redefinition, which
// warns on every TinyUSB translation unit -- and would silently win over the
// SDK's value if the two ever differed. Only supply them as fallbacks.
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU                 OPT_MCU_RP2040   // covers RP2350 in TinyUSB
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                  OPT_OS_PICO
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG               0
#endif

#define BOARD_TUD_RHPORT             0        // native USB controller
#define BOARD_TUH_RHPORT             1        // PIO-USB

#define CFG_TUD_ENABLED              1
#define CFG_TUH_ENABLED              1
#define CFG_TUH_RPI_PIO_USB          1
#define CFG_TUD_MAX_SPEED            OPT_MODE_FULL_SPEED
#define CFG_TUH_MAX_SPEED            OPT_MODE_FULL_SPEED

// PIO-USB pin configuration (D- must be D+ plus one).
//
// PIO_USB_DP_PIN_DEFAULT is the macro Pico-PIO-USB actually reads when it
// builds PIO_USB_DEFAULT_CONFIG. PICO_PIO_USB_PIN_DP is NOT read by the
// library -- it was my invention and had no effect. Working Fruit Jam
// projects set PIO_USB_DP_PIN_DEFAULT here; usb_host.cpp also assigns
// pio_cfg.pin_dp explicitly, so the pin is correct either way.
#define PIO_USB_DP_PIN_DEFAULT       1

// PICO_PIO_USB_USE_TINYUSB_MEM used to be defined here. It is not a macro the
// library reads -- another name I invented. The real one is PIO_USB_USE_TINYUSB,
// now set as a compile definition in CMakeLists.txt.

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN           __attribute__((aligned(4)))
#endif

// ---------------------------------------------------------------------------
// Device stack
// ---------------------------------------------------------------------------
#define CFG_TUD_ENDPOINT0_SIZE       64
#define CFG_TUD_MSC                  1
#define CFG_TUD_CDC                  0
#define CFG_TUD_HID                  0
#define CFG_TUD_MIDI                 0
#define CFG_TUD_VENDOR               0

// A large MSC buffer keeps drag-and-drop transfers from stalling while the
// emulator holds the SPI bus.
#define CFG_TUD_MSC_EP_BUFSIZE       4096

// ---------------------------------------------------------------------------
// Host stack
// ---------------------------------------------------------------------------
#define CFG_TUH_HUB                  1        // the CH334F is a hub
#define CFG_TUH_HID                  4        // up to 4 gamepads
#define CFG_TUH_CDC                  0
#define CFG_TUH_MSC                  0
#define CFG_TUH_VENDOR               0

// Root port + hub ports.
#define CFG_TUH_DEVICE_MAX           (CFG_TUH_HUB + 3)
#define CFG_TUSB_RHPORT0_MODE        (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUH_ENUMERATION_BUFSIZE  256
#define CFG_TUH_HID_EPIN_BUFSIZE     64
#define CFG_TUH_HID_EPOUT_BUFSIZE    64

#ifdef __cplusplus
}
#endif
