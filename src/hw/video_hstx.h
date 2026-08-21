// video_hstx.h -- HSTX DVI output
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

void      video_init(void);
uint16_t *video_framebuffer(void);      // 320x240 RGB565
uint32_t  video_frame_count(void);
void      video_wait_vsync(void);

void video_clear(uint16_t colour);
void video_fill_rect(int x, int y, int w, int h, uint16_t colour);
void video_set_pixel(int x, int y, uint16_t colour);

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
