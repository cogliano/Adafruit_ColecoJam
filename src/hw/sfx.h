// sfx.h -- one-shot sound effect playback, mixed over whatever else is playing
//
// Used by the title sequence for the start-up chime. Deliberately a mixer
// rather than a player that owns the audio path: the emulator's PSG output and
// this can be live at the same time, and neither has to know about the other.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Start the start-up chime from the beginning. Playing it again while it is
// still sounding restarts it rather than layering a second copy.
void sfx_play_start(void);

// True while there is still something to play.
bool sfx_active(void);

// Stop immediately.
void sfx_stop(void);

// Mix the remaining effect into `count` mono samples at AUDIO_SAMPLE_RATE,
// adding to whatever is already in the buffer and saturating rather than
// wrapping. Does nothing when no effect is playing, so it is safe to call
// unconditionally from an audio loop.
void sfx_mix(int16_t *buf, int count);

// Fill a buffer with the effect alone, silence where it has finished. For
// callers with no other audio source, such as the title sequence.
void sfx_render(int16_t *buf, int count);
