// sdcard.h -- SPI microSD access
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

bool     sd_init(void);

// Why the last sd_init() failed:
//   0 = no error
//   1 = nothing responded to CMD0 (card absent, not seated, or wiring)
//   2 = card answered but failed the CMD8 voltage/pattern check
//   3 = card present and talking but never finished initialising
//   4 = card held the bus busy and never went ready
int      sd_last_error(void);
bool     sd_ready(void);
uint32_t sd_block_count(void);
bool     sd_read_blocks(uint32_t lba, uint8_t *dst, uint32_t count);
bool     sd_write_blocks(uint32_t lba, const uint8_t *src, uint32_t count);
