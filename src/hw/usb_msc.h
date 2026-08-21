// usb_msc.h -- USB mass storage device exposing the SD card
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

// Enable or disable the mass-storage device before usb_msc_init(). When
// disabled the USB-C port does not enumerate at all, so no drive appears on a
// host and the emulator keeps sole use of the SD card.
void usb_msc_set_enabled(bool enabled);
bool usb_msc_enabled(void);

void usb_msc_init(void);
void usb_msc_task(void);
bool usb_msc_host_connected(void);   // true while a PC has the volume mounted
bool usb_msc_media_dirty(void);      // true if the host wrote since last check
