// psram.h -- the board's 8 MB PSRAM, which holds the cartridge
//
// Every cartridge is loaded here, not just bank-switched ones: the 32 KB of
// SRAM a local buffer used to cost is worth more than the copy.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Check the PSRAM and the cartridge buffer placed in it, by writing and
// reading a pattern at both ends. The SDK has already brought the chip up
// during runtime init, so this only verifies it. Returns false if it is absent or does not read back correctly, in
// which case MegaCart ROMs are refused with a clear message rather than
// loading as corrupt data. Safe to call more than once.
bool cj_psram_setup(void);

// Memory-mapped base, or nullptr until cj_psram_setup() has succeeded.
uint8_t *cj_psram_base(void);
size_t   cj_psram_bytes(void);
bool     cj_psram_ready(void);

// Scratch past the cartridge buffer, for anything else that would otherwise
// cost SRAM -- currently the EEPROM. CJ_PSRAM_SCRATCH_BYTES is verified at
// startup along with the cartridge area.
#define CJ_PSRAM_SCRATCH_BYTES  (64u * 1024u)
uint8_t *cj_psram_scratch(void);
