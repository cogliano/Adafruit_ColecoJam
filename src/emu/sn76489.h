// sn76489.h -- Texas Instruments SN76489A PSG (3 tone + 1 noise)
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#define PSG_CLOCK_HZ   3579545      // same crystal as the Z80
#define PSG_SAMPLE_HZ  44100

struct SN76489 {
    uint16_t tone_reg[4];       // 10-bit period for ch 0-2, 3-bit ctrl for ch 3
    uint8_t  vol_reg[4];        // 4-bit attenuation, 0 = loudest, 15 = off
    uint8_t  latched;           // channel currently selected by a latch byte
    bool     latched_vol;

    int32_t  counter[4];        // down-counters in PSG clock/16 units
    int8_t   output[4];         // +1 / -1 square state

    uint16_t lfsr;              // noise shift register
    uint8_t  noise_ctrl;

    int32_t  frac;              // clock-to-sample accumulator (16.16)
};

extern SN76489 psg;

void psg_init(void);
void psg_reset(void);
void psg_write(uint8_t value);

// Renders `count` mono samples at PSG_SAMPLE_HZ into a signed 16-bit buffer.
void psg_render(int16_t *buf, int count);
