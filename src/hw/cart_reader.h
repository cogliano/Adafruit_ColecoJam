// cart_reader.h -- ColecoVision cartridge reader shield
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
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
