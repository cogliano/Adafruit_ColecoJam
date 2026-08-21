// sn76489.cpp -- SN76489A programmable sound generator
//
// The chip divides its input clock by 16 to get the tone counter clock. We run
// the counters at that rate and resample down to 44.1 kHz with a 16.16 fixed
// point accumulator, averaging every counter tick that falls inside a sample
// period. That averaging is what keeps the high-frequency channels (which
// oscillate faster than the sample rate) from aliasing into audible whine.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sn76489.h"
#include <string.h>

#ifdef PICO_ON_DEVICE
#include "pico.h"
#define HOT __not_in_flash_func
#else
#define HOT(x) x
#endif

SN76489 psg;

// Logarithmic attenuation table: 2 dB per step, 15 = silence.
static const int16_t vol_table[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031,  819,  650,  516,  410,  326,    0
};

#define PSG_TICK_HZ  (PSG_CLOCK_HZ / 16)     // 223,721 Hz

void psg_init(void) { psg_reset(); }

void psg_reset(void) {
    memset(&psg, 0, sizeof(psg));
    for (int i = 0; i < 4; i++) {
        psg.tone_reg[i] = 0;
        psg.vol_reg[i]  = 0x0F;     // all channels silent at reset
        psg.counter[i]  = 0;
        psg.output[i]   = 1;
    }
    psg.lfsr    = 0x8000;
    psg.latched = 0;
    psg.frac    = 0;
}

void psg_write(uint8_t value) {
    if (value & 0x80) {
        // Latch/data byte: %1cctdddd
        psg.latched     = (uint8_t)((value >> 5) & 0x03);
        psg.latched_vol = (value & 0x10) != 0;
        if (psg.latched_vol) {
            psg.vol_reg[psg.latched] = (uint8_t)(value & 0x0F);
        } else {
            psg.tone_reg[psg.latched] =
                (uint16_t)((psg.tone_reg[psg.latched] & 0x3F0) | (value & 0x0F));
            if (psg.latched == 3) {
                psg.noise_ctrl = (uint8_t)(value & 0x07);
                psg.lfsr = 0x8000;
            }
        }
    } else {
        // Data byte: %0-dddddd (upper 6 bits of the tone period)
        if (psg.latched_vol) {
            psg.vol_reg[psg.latched] = (uint8_t)(value & 0x0F);
        } else if (psg.latched == 3) {
            psg.noise_ctrl = (uint8_t)(value & 0x07);
            psg.lfsr = 0x8000;
        } else {
            psg.tone_reg[psg.latched] =
                (uint16_t)((psg.tone_reg[psg.latched] & 0x00F) | ((value & 0x3F) << 4));
        }
    }
}

// One tick of the /16 clock.
static inline void HOT(psg_tick)(void) {
    for (int ch = 0; ch < 3; ch++) {
        if (--psg.counter[ch] <= 0) {
            uint16_t period = psg.tone_reg[ch];
            // A period of 0 or 1 is above the audible range; the real chip
            // outputs a steady DC level there, which is how games mute a
            // channel without touching the volume register.
            psg.counter[ch] = period ? period : 1;
            if (period > 1) psg.output[ch] = (int8_t)-psg.output[ch];
            else            psg.output[ch] = 1;
        }
    }

    if (--psg.counter[3] <= 0) {
        static const uint16_t noise_periods[4] = { 0x10, 0x20, 0x40, 0 };
        uint16_t p = noise_periods[psg.noise_ctrl & 0x03];
        psg.counter[3] = p ? p : (psg.tone_reg[2] ? psg.tone_reg[2] : 1);

        // 16-bit LFSR, taps at bits 0 and 3 for the SN76489A "white" mode.
        uint16_t feedback;
        if (psg.noise_ctrl & 0x04)
            feedback = (uint16_t)(((psg.lfsr & 0x0009) && ((psg.lfsr & 0x0009) ^ 0x0009)) ? 1 : 0);
        else
            feedback = (uint16_t)(psg.lfsr & 1);

        psg.lfsr = (uint16_t)((psg.lfsr >> 1) | (feedback << 15));
        psg.output[3] = (int8_t)((psg.lfsr & 1) ? 1 : -1);
    }
}

static inline int32_t HOT(psg_mix)(void) {
    int32_t s = 0;
    for (int ch = 0; ch < 4; ch++)
        s += psg.output[ch] * vol_table[psg.vol_reg[ch] & 0x0F];
    return s;
}

void HOT(psg_render)(int16_t *buf, int count) {
    // Ticks of the /16 clock per output sample, in 16.16 fixed point.
    const int32_t step = (int32_t)(((int64_t)PSG_TICK_HZ << 16) / PSG_SAMPLE_HZ);

    for (int i = 0; i < count; i++) {
        psg.frac += step;
        int ticks = psg.frac >> 16;
        psg.frac &= 0xFFFF;

        int32_t acc = 0;
        if (ticks <= 0) {
            acc = psg_mix();
            ticks = 1;
        } else {
            for (int t = 0; t < ticks; t++) {
                psg_tick();
                acc += psg_mix();
            }
        }

        int32_t s = acc / ticks / 4;          // /4 keeps four channels in range
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}
