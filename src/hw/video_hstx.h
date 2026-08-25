// video_hstx.h -- HSTX DVI output
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

void      video_init(void);
uint16_t *video_framebuffer(void);      // 320x240 RGB565
// Frames scanned out since init. Named to avoid pico_hdmi's own
// `extern volatile uint32_t video_frame_count`, which that library exports as
// a variable -- a function of the same name is a hard compile error when both
// headers are visible.
uint32_t  video_frames_rendered(void);
void      video_wait_vsync(void);

void video_clear(uint16_t colour);
void video_fill_rect(int x, int y, int w, int h, uint16_t colour);
void video_set_pixel(int x, int y, uint16_t colour);

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
