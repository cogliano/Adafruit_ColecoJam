// coleco.h -- ColecoVision system bus, controllers and frame driver
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define CV_BIOS_SIZE   0x2000        // 8 KB
#define CV_RAM_SIZE    0x0400        // 1 KB, mirrored eight times at 0x6000
#define CV_ROM_MAX     0x8000        // 32 KB standard cartridge window

// Master timing. 10.738635 MHz / 3 = 3.579545 MHz Z80.
// One TMS9918A scanline is 342 VDP dots = 228 Z80 T-states.
#define CV_CYCLES_PER_LINE   228
#define CV_LINES_PER_FRAME   262
#define CV_CYCLES_PER_FRAME  (CV_CYCLES_PER_LINE * CV_LINES_PER_FRAME)

// Logical ColecoVision controller state, one per port. Set by input.cpp.
enum {
    CV_JOY_UP     = 1 << 0,
    CV_JOY_RIGHT  = 1 << 1,
    CV_JOY_DOWN   = 1 << 2,
    CV_JOY_LEFT   = 1 << 3,
    CV_BTN_RIGHT  = 1 << 4,   // right side action button (fire)
    CV_BTN_LEFT   = 1 << 5,   // left side action button
};

// Keypad key indices. CV_KEY_NONE means nothing held.
enum {
    CV_KEY_0 = 0, CV_KEY_1, CV_KEY_2, CV_KEY_3, CV_KEY_4,
    CV_KEY_5, CV_KEY_6, CV_KEY_7, CV_KEY_8, CV_KEY_9,
    CV_KEY_STAR, CV_KEY_HASH,
    CV_KEY_NONE = 0xFF,
};

struct CVController {
    uint8_t joy;        // bitmask of CV_JOY_* / CV_BTN_*
    uint8_t keypad;     // CV_KEY_* or CV_KEY_NONE
};

extern CVController cv_pad[2];

// NMIs delivered since reset. The vertical blank drives every ColecoVision
// game's main loop, so this advancing at ~60/s is the sign of a healthy
// machine; frozen while the CPU still runs means the game is waiting on an
// interrupt that stopped coming.
extern uint32_t cv_nmi_count;

// Latched interrupt-line state used for edge detection.
bool cv_prev_irq(void);

// Captured when the last NMI was raised: where the CPU was, the status-read
// count at that moment, and the BIOS RAM dispatch hook.
extern uint16_t cv_last_nmi_pc;
extern uint32_t cv_reads_at_nmi;
extern uint16_t cv_nmi_vector;

bool cv_init(const uint8_t *bios, uint32_t bios_len);
void cv_load_rom(const uint8_t *rom, uint32_t len);
void cv_reset(void);

// Runs a single scanline of the machine: CV_CYCLES_PER_LINE of Z80 time plus
// the matching VDP line. `line_dest` receives 256 RGB565 pixels, or may be
// null for lines that are not being displayed.
void cv_run_scanline(uint16_t *line_dest);

// Convenience: run a whole frame into a 256x192 RGB565 buffer with the given
// stride in pixels.
void cv_run_frame(uint16_t *fb, int stride);
