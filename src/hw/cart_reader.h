// cart_reader.h -- ColecoVision cartridge reader shield
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// For ENABLE_MEGACART, used below.
#include "config.h"
#include <stdbool.h>

// True when a cartridge is seated and its header signature checks out.
bool cart_present(void);

// Reads the cartridge into `dst`. Returns the number of bytes read, which will
// be a multiple of 8 KB (8/16/24/32 KB depending on how many ROM banks the
// cartridge populates). Returns 0 on failure.
uint32_t cart_read(uint8_t *dst, uint32_t max_len);

// The first two bytes seen through each of the four chip selects during the
// last cart_present() call: [cs0 b0, cs0 b1, cs1 b0, cs1 b1, ...]. Returns how
// many bytes were copied. A cartridge should start AA 55 or 55 AA.
int cart_probe_bytes(uint8_t *dst, int max);

// Which chip select carried that header, or -1 if none did. A value other
// than 0 means the chip select ordering differs from the assumed one.
int cart_header_bank(void);

// Diagnostics from the last cart_read(): the size before and after trimming,
// and one character per chip select (P populated, B blank but real, U floating
// so skipped, '-' not reached).
struct CartReadStatus {
    uint32_t raw;
    uint32_t trimmed;
    char     banks[5];
    uint8_t  uniform_value;   // the repeated byte, when a bank looked uniform
};
extern CartReadStatus cart_status;

#if ENABLE_MEGACART
// Progress callback for a long dump: called with bytes done and the total.
typedef void (*cart_progress_fn)(uint32_t done, uint32_t total);

// Dump a bank-switched cartridge into `dst`, which must be large enough for
// the whole ROM -- in practice the PSRAM buffer. Returns the number of bytes
// read, or 0 if the cartridge is not bank-switched (use cart_read() for that).
//
// The banking is on the cartridge, so this drives the bank select itself,
// discovers how many banks there are, and reads each one.
uint32_t cart_read_banked(uint8_t *dst, uint32_t max_len, cart_progress_fn progress);
#endif
