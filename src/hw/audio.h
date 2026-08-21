// audio.h -- I2S audio output
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

bool audio_init(void);

// Did the TLV320DAC3100 acknowledge on I2C during init? False means the codec
// was never configured, so there will be silence no matter what the emulator
// produces.
bool     audio_codec_ok(void);

// TLV320 DAC flag register (page 0, 0x25) read back after init.
// Bit 7 = left DAC powered up, bit 3 = right DAC powered up. These only set
// when the part has a valid clock, so a zero here means the PLL never locked.
uint8_t  audio_codec_flags(void);

// Page 1 registers read back after init: 0x23 (output mixer routing, expect
// 0x44) and 0x1F (headphone driver power, expect 0xC4). The second one matters
// because that write was missing altogether for several builds.
uint8_t  audio_route_reg(void);
uint8_t  audio_hpvol_reg(void);

// Count of buffers handed to the I2S DMA. If this is not climbing, the DMA
// chain is not running and nothing is being clocked out at all -- a different
// fault from a codec that is mute.
uint32_t audio_buffers_sent(void);

// DMA completion interrupts seen, and whether the I2S state machine is
// actually draining its FIFO. Together these separate "DMA stopped" from
// "PIO never started".
uint32_t audio_irq_count(void);
bool     audio_i2s_running(void);

// Program counter, TX FIFO level and DMA busy bits. See audio.cpp.
void     audio_debug(uint8_t *pc, uint8_t *fifo, uint8_t *busy);

// Diagnostic path: push samples straight to the state machine, bypassing DMA.
// Blocks until the SM accepts them. Used when AUDIO_CPU_FEED is set.
void     audio_push_blocking(const int16_t *mono, int count);
void audio_set_volume(int percent);

// Returns 0 or 1 when that half-buffer needs refilling, or -1 if neither does.
int  audio_buffer_needed(void);
void audio_submit(int half, const int16_t *mono, int count);
