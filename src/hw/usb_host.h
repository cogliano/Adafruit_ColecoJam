// usb_host.h -- USB HID gamepad host
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#define MAX_PADS 4

// Records the PIO-USB configuration. Must be called on core 0 BEFORE any
// TinyUSB initialisation (i.e. before usb_msc_init()).
void     usb_host_prepare(void);

// Starts the host stack. Must run on the core that will call usb_host_task().
void     usb_host_init(void);
void     usb_host_task(void);

// Retry HID report requests that failed earlier. Implemented in hid_app.cpp
// and called from usb_host_task(); see note (f) there.
void     hid_app_retry_pending(void);
int      usb_host_pad_count(void);
int      usb_host_hid_seen(void);   // HID interfaces mounted, gamepad or not
int      usb_host_dev_seen(void);   // USB devices attached, hub included
bool     usb_host_init_done(void);  // did usb_host_init() actually finish?

// How far usb_host_init() got, for diagnosing a hang on core 1:
//   1 = prepare entered, powering the USB-A ports
//   2 = 5V enabled and settled
//   3 = config built, DMA channel 0 reserved for PIO-USB
//   4 = tuh_configure() returned  (still on core 0)
//   5 = usb_host_init() entered   (on the host core)
//   6 = DMA channel 0 released back for PIO-USB to claim
//   7 = tuh_init() returned -- complete
int      usb_host_init_step(void);

// Copies the most recent raw HID report from a pad. Returns the byte count.
// Displayed by the menu so an unrecognised controller can be decoded from real
// data rather than guessed at.
int      usb_host_raw_report(int index, uint8_t *dst, int max);
int      usb_host_report_len(int index);   // true report length in bytes
uint32_t usb_host_report_count(void);      // total HID reports received
uint16_t usb_host_raw_buttons(int index);   // MENU_* bitmask, for the menu
void     usb_host_update_coleco(void);      // maps pads 0/1 into cv_pad[]

// Is a USB keyboard attached? It never occupies a controller port: its keypad
// digits, arrows and fire keys merge into port 1, so a gamepad stays player 1
// and the keyboard supplies the 12-key ColecoVision keypad. This also changes
// the pad mapping -- see map_pad() in usb_host.cpp.
bool     usb_host_keyboard_connected(void);

// Short name of the pad in a port ("DS4", "X360", "MSNES", ...), or nullptr.
// Set by hid_app.cpp when it recognises the device.
const char *usb_host_pad_name(int index);

// Called by hid_app.cpp for every HID report received, before decoding, so the
// SHOW_HID_DEBUG menu footer has real bytes to display. Not for general use.
void     usb_host_note_report(unsigned char dev_addr, unsigned char instance,
                              const uint8_t *report, unsigned short len);

// Menu-facing button constants (must match usb_host.cpp).
enum {
    MENU_UP     = 1 << 0,  MENU_DOWN  = 1 << 1,
    MENU_LEFT   = 1 << 2,  MENU_RIGHT = 1 << 3,
    MENU_A      = 1 << 4,  MENU_B     = 1 << 5,
    MENU_X      = 1 << 6,  MENU_Y     = 1 << 7,
    MENU_L      = 1 << 8,  MENU_R     = 1 << 9,
    MENU_SELECT = 1 << 10, MENU_START = 1 << 11,
};
