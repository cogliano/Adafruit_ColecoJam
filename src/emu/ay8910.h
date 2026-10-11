// ay8910.h -- AY-3-8910, the Super Game Module's second sound chip
//
// Three square-wave tone channels, one noise generator and one envelope
// generator, any of which can be routed to any channel. Independent of the
// ColecoVision's own SN76489, and on an SGM machine both are audible at once.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

void ay_init(void);
void ay_reset(void);

// Port 0x50 latches the register number; 0x51 writes it; 0x52 reads it back.
void    ay_write_address(uint8_t reg);
void    ay_write_data(uint8_t value);
uint8_t ay_read_data(void);

// Mix `count` mono samples at AUDIO_SAMPLE_RATE into buf, adding to whatever
// is already there and saturating rather than wrapping. Silent until a game
// writes to the chip, so it costs nothing on a standard cartridge.
void ay_mix(int16_t *buf, int count);

// True once anything has been written to the chip. Used to keep the mixing
// out of the way entirely for non-SGM games.
bool ay_in_use(void);
