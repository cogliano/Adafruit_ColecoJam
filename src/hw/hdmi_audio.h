// hdmi_audio.h -- emulator sound carried in the HDMI stream
//
// Implemented in video_hdmi.cpp, because the audio data islands are produced
// by the same pico_hdmi instance that drives the picture. Only meaningful when
// VIDEO_DRIVER is VIDEO_DRIVER_PICO_HDMI and AUDIO_SINK is AUDIO_SINK_HDMI.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

void     hdmi_audio_init(void);

// Hands mono samples to the HDMI data island queue, four per packet. Returns
// how many were consumed -- fewer than offered when the queue is full, so the
// caller keeps the remainder for next time rather than dropping it.
int      hdmi_audio_submit(const int16_t *mono, int count);

// Packets waiting to go out. The emulator tops this up towards
// HDMI_AUDIO_QUEUE_TARGET each frame.
uint32_t hdmi_audio_queue_level(void);

// Times the queue ran dry and the library sent a silence packet instead.
// Climbing during play means the emulator is not feeding it fast enough.
uint32_t hdmi_audio_underruns(void);
