// tms9918.h -- Texas Instruments TMS9918A / TMS9928A VDP
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VDP_VRAM_SIZE     0x4000     // 16 KB
#define VDP_ACTIVE_W      256
#define VDP_ACTIVE_H      192
#define VDP_LINES_NTSC    262
#define VDP_LINES_PAL     313

struct TMS9918 {
    uint8_t  vram[VDP_VRAM_SIZE];
    uint8_t  reg[8];
    uint8_t  status;
    uint8_t  read_latch;             // holds the pre-fetched VRAM byte
    uint16_t address;
    uint8_t  first_byte;             // latched low byte of a control write
    bool     second_write;

    int      line;                   // current scanline (0..261)
    bool     irq;                    // INT output state (wired to Z80 /NMI)

    // Per-line 8-bit colour-index output. Index 0 is transparent -> backdrop.
    uint8_t  linebuf[VDP_ACTIVE_W];
};

extern TMS9918 vdp;

void vdp_init(void);
void vdp_reset(void);

// Runs one scanline: renders it into `dest` (which must be at least 256 bytes
// of RGB565, i.e. uint16_t[256]) when the line is inside the active display,
// then advances the internal line counter. Returns true when the line just
// completed was the one that raises the frame interrupt.
bool vdp_scanline(uint16_t *dest);

uint8_t vdp_read_data(void);
uint8_t vdp_read_status(void);
void    vdp_write_data(uint8_t value);
void    vdp_write_control(uint8_t value);

extern uint16_t vdp_palette_rgb565[16];
