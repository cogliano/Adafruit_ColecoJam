// sfx.cpp -- one-shot sound effect playback
//
// The sample data is stored at 11025 Hz while the audio path runs at 44100, so
// playback interpolates. 44100 / 11025 is exactly 4, but the step is kept in
// 16.16 fixed point anyway rather than hard-coding a 4x loop: it costs nothing
// here, and it means a differently-rated sample, or a change to
// AUDIO_SAMPLE_RATE, still plays at the right pitch instead of silently
// playing fast or slow.
//
// Linear interpolation rather than repeating each sample four times. Holding a
// sample creates images of the signal around the sample rate, which on a quiet
// chime is audible as a hard edge; interpolating costs one multiply per sample
// and removes most of it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sfx.h"
#include "config.h"
#if TITLE_SOUND
#include "../ui/sound_start.h"
#endif

namespace {

const int16_t *s_data = nullptr;
uint32_t       s_len  = 0;        // samples in s_data
uint32_t       s_pos  = 0;        // 16.16 position within s_data
uint32_t       s_step = 0;        // 16.16 increment per output sample

// One output sample, or 0 when finished. Advances the position.
inline int32_t next_sample(void) {
    const uint32_t idx = s_pos >> 16;
    if (!s_data || idx >= s_len) { s_data = nullptr; return 0; }

    const int32_t a = s_data[idx];
    const int32_t b = (idx + 1 < s_len) ? s_data[idx + 1] : 0;
    const int32_t f = (int32_t)(s_pos & 0xFFFF);

    s_pos += s_step;
    return a + (((b - a) * f) >> 16);
}

inline int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

} // namespace

void sfx_play_start(void) {
#if TITLE_SOUND
    s_data = snd_start;
    s_len  = SND_START_LEN;
    s_pos  = 0;
    // 16.16 step: how far through the sample data each output sample moves.
    s_step = (uint32_t)(((uint64_t)SND_START_RATE << 16) / AUDIO_SAMPLE_RATE);
#endif
}

bool sfx_active(void) {
    return s_data != nullptr && (s_pos >> 16) < s_len;
}

void sfx_stop(void) { s_data = nullptr; }

void sfx_mix(int16_t *buf, int count) {
    if (!sfx_active()) return;
    for (int i = 0; i < count; i++)
        buf[i] = clamp16((int32_t)buf[i] + next_sample());
}

void sfx_render(int16_t *buf, int count) {
    for (int i = 0; i < count; i++)
        buf[i] = sfx_active() ? clamp16(next_sample()) : 0;
}
