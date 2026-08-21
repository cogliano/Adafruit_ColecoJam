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
