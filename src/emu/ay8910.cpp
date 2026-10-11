// ay8910.cpp -- AY-3-8910 emulation for the Super Game Module
//
// Clocked at 3.579545 MHz on the SGM, the same crystal as the Z80. The model
// works in generator "ticks" of clock/8 -- about 447 kHz, or ten ticks per
// output sample at 44.1 kHz -- rather than in raw clocks. Stepping tick by
// tick is cheap at that rate and avoids the aliasing a per-sample
// approximation gives on the short tone periods games use for effects.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ay8910.h"
#include "config.h"
#include <string.h>

#define AY_CLOCK   3579545u

// Generators step at clock/8, not clock/16.
//
// The datasheet quotes tone frequency as clock / (16 x TP), and the counter
// toggles the output once every TP steps -- but a toggle is half a cycle, so
// the wave's period is 2 x TP steps. Stepping at clock/16 therefore produces
// clock / (32 x TP): everything an octave flat. clock/8 makes the arithmetic
// come out at the quoted figure.
#define AY_TICK_HZ (AY_CLOCK / 8u)

namespace {

uint8_t  reg[16];
uint8_t  latched;                        // register selected by port 0x50
bool     used;                           // has a game touched the chip?

uint32_t tone_cnt[3];
uint8_t  tone_out[3];

uint32_t noise_cnt;
uint32_t lfsr;
uint8_t  noise_out;

uint32_t env_cnt;
int      env_step;                       // 0..31
bool     env_attack, env_hold, env_alt, env_cont, env_holding;

uint32_t tick_acc;                       // 16.16 accumulator of ticks
uint32_t tick_inc;                       // ticks per output sample, 16.16

// Amplitudes of the 16 volume steps. The chip is logarithmic, roughly 3 dB a
// step, so a linear ramp sounds wrong -- quiet passages vanish and loud ones
// bunch up at the top. Scaled so three channels at full volume stay inside
// 16-bit range with room for the SN76489 mixed on top.
const int16_t vol_table[16] = {
       0,   74,  110,  156,  227,  331,  454,  733,
     906, 1418, 1889, 2410, 3055, 3681, 4543, 5356,
};

inline uint32_t tone_period(int ch) {
    const uint32_t p = (uint32_t)reg[ch * 2] | ((uint32_t)(reg[ch * 2 + 1] & 0x0F) << 8);
    return p ? p : 1;                    // 0 behaves as 1 in the hardware
}

inline uint32_t noise_period(void) {
    const uint32_t p = reg[6] & 0x1F;
    return p ? p : 1;
}

inline uint32_t env_period(void) {
    const uint32_t p = (uint32_t)reg[11] | ((uint32_t)reg[12] << 8);
    return p ? p : 1;
}

// One envelope step. The four control bits in register 13 combine into the
// chip's sixteen shapes; this is the usual reading of them.
void env_advance(void) {
    if (env_holding) return;

    env_step++;
    if (env_step <= 31) return;

    if (!env_cont) {
        // Single ramp, then silence.
        env_step    = 31;
        env_attack  = false;
        env_holding = true;
        return;
    }
    if (env_alt) env_attack = !env_attack;
    if (env_hold) {
        env_step    = 31;
        env_holding = true;
    } else {
        env_step = 0;
    }
}

inline int env_level(void) {
    const int s = env_holding && !env_cont ? 31 : env_step;
    return env_attack ? s / 2 : 15 - (s / 2);
}

// Advance every generator by one tick.
inline void tick(void) {
    for (int ch = 0; ch < 3; ch++) {
        if (++tone_cnt[ch] >= tone_period(ch)) {
            tone_cnt[ch] = 0;
            tone_out[ch] ^= 1;
        }
    }

    if (++noise_cnt >= noise_period()) {
        noise_cnt = 0;
        // 17-bit LFSR, taps at bits 0 and 3.
        const uint32_t bit = ((lfsr ^ (lfsr >> 3)) & 1u);
        lfsr = (lfsr >> 1) | (bit << 16);
        noise_out = (uint8_t)(lfsr & 1u);
    }

    // The envelope period is 256 clocks, which is 32 ticks at clock/8.
    if (++env_cnt >= env_period() * 32u) {
        env_cnt = 0;
        env_advance();
    }
}

// Sum of the three channels for the current generator state.
inline int32_t output(void) {
    const uint8_t mix = reg[7];
    int32_t out = 0;

    for (int ch = 0; ch < 3; ch++) {
        // A set bit in register 7 DISABLES that source, so a disabled source
        // reads as a constant 1 and leaves the other one in control.
        const uint8_t t = ((mix >> ch)       & 1u) ? 1u : tone_out[ch];
        const uint8_t n = ((mix >> (ch + 3)) & 1u) ? 1u : noise_out;
        if (!(t & n)) continue;

        const uint8_t v = reg[8 + ch];
        const int level = (v & 0x10) ? env_level() : (v & 0x0F);
        out += vol_table[level & 0x0F];
    }
    return out;
}

inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

} // namespace

void ay_init(void) {
    tick_inc = (uint32_t)(((uint64_t)AY_TICK_HZ << 16) / AUDIO_SAMPLE_RATE);
    ay_reset();
}

void ay_reset(void) {
    memset(reg, 0, sizeof(reg));
    reg[7] = 0x3F;                       // everything disabled: silence
    memset(tone_cnt, 0, sizeof(tone_cnt));
    memset(tone_out, 0, sizeof(tone_out));
    latched = 0;
    used    = false;
    noise_cnt = 0;
    lfsr      = 1;                       // must not be zero, or it never runs
    noise_out = 0;
    env_cnt = 0; env_step = 0;
    env_attack = env_hold = env_alt = env_cont = env_holding = false;
    tick_acc = 0;
}

void ay_write_address(uint8_t r) { latched = (uint8_t)(r & 0x0F); }

uint8_t ay_read_data(void) { return reg[latched]; }

void ay_write_data(uint8_t value) {
    used = true;
    reg[latched] = value;

    if (latched == 13) {
        // Writing the shape register always restarts the envelope, even with
        // the same value -- games use that to retrigger a note.
        env_cont    = (value & 0x08) != 0;
        env_attack  = (value & 0x04) != 0;
        env_alt     = (value & 0x02) != 0;
        env_hold    = (value & 0x01) != 0;
        env_step    = 0;
        env_cnt     = 0;
        env_holding = false;
    }
}

bool ay_in_use(void) { return used; }

void ay_mix(int16_t *buf, int count) {
    if (!used) return;                   // never written: nothing to add

    for (int i = 0; i < count; i++) {
        tick_acc += tick_inc;
        while (tick_acc >= (1u << 16)) {
            tick_acc -= (1u << 16);
            tick();
        }
        buf[i] = clamp16((int32_t)buf[i] + output());
    }
}
