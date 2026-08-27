// tms9918.cpp -- TMS9918A VDP
//
// Renders one scanline at a time straight into an RGB565 line of the HSTX
// framebuffer. Rendering per line (rather than per frame) keeps peak RAM low
// and lets mid-frame register changes work, which several ColecoVision titles
// rely on for status bars.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tms9918.h"
#include <string.h>

#ifdef PICO_ON_DEVICE
#include "pico.h"
#define HOT __not_in_flash_func
#else
#define HOT(x) x
#endif

TMS9918 vdp;

// The classic TMS9918A palette, converted to RGB565.
// (Values are the widely used Sean Young / TI datasheet RGB set.)
static const uint8_t pal_rgb[16][3] = {
    {   0,   0,   0 },   // 0  transparent (shown as backdrop)
    {   0,   0,   0 },   // 1  black
    {  33, 200,  66 },   // 2  medium green
    {  94, 220, 120 },   // 3  light green
    {  84,  85, 237 },   // 4  dark blue
    { 125, 118, 252 },   // 5  light blue
    { 212,  82,  77 },   // 6  dark red
    {  66, 235, 245 },   // 7  cyan
    { 252,  85,  84 },   // 8  medium red
    { 255, 121, 120 },   // 9  light red
    { 212, 193,  84 },   // 10 dark yellow
    { 230, 206, 128 },   // 11 light yellow
    {  33, 176,  59 },   // 12 dark green
    { 201,  91, 186 },   // 13 magenta
    { 204, 204, 204 },   // 14 grey
    { 255, 255, 255 },   // 15 white
};

uint16_t vdp_palette_rgb565[16];
#define pal565 vdp_palette_rgb565

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// --- register helpers ------------------------------------------------------
#define R0 vdp.reg[0]
#define R1 vdp.reg[1]
#define R2 vdp.reg[2]
#define R3 vdp.reg[3]
#define R4 vdp.reg[4]
#define R5 vdp.reg[5]
#define R6 vdp.reg[6]
#define R7 vdp.reg[7]

static inline bool screen_on(void)  { return (R1 & 0x40) != 0; }
static inline bool irq_enabled(void){ return (R1 & 0x20) != 0; }
static inline int  mode(void) {
    // M1 = R1 bit4, M2 = R1 bit3, M3 = R0 bit1
    int m = 0;
    if (R0 & 0x02) m |= 1;    // M3 -> Graphics II
    if (R1 & 0x08) m |= 2;    // M2 -> Multicolour
    if (R1 & 0x10) m |= 4;    // M1 -> Text
    return m;
}

void vdp_init(void) {
    for (int i = 0; i < 16; i++)
        pal565[i] = rgb565(pal_rgb[i][0], pal_rgb[i][1], pal_rgb[i][2]);
    vdp_reset();
}

void vdp_reset(void) {
    memset(vdp.vram, 0, sizeof(vdp.vram));
    memset(vdp.reg, 0, sizeof(vdp.reg));
    vdp.status = 0;
    vdp.address = 0;
    vdp.read_latch = 0;
    vdp.first_byte = 0;
    vdp.second_write = false;
    vdp.line = 0;
    vdp.irq = false;
}

// ---------------------------------------------------------------------------
// CPU-side ports
// ---------------------------------------------------------------------------
uint8_t vdp_read_data(void) {
    uint8_t v = vdp.read_latch;
    vdp.read_latch = vdp.vram[vdp.address & 0x3FFF];
    vdp.address = (uint16_t)((vdp.address + 1) & 0x3FFF);
    vdp.second_write = false;
    return v;
}

uint8_t vdp_read_status(void) {
    uint8_t v = vdp.status;
    vdp.status &= 0x1F;              // clear INT, 5S and collision
    vdp.second_write = false;
    vdp.irq = false;
    return v;
}

void vdp_write_data(uint8_t value) {
    vdp.vram[vdp.address & 0x3FFF] = value;
    vdp.read_latch = value;
    vdp.address = (uint16_t)((vdp.address + 1) & 0x3FFF);
    vdp.second_write = false;
}

void vdp_write_control(uint8_t value) {
    if (!vdp.second_write) {
        vdp.first_byte = value;
        vdp.address = (uint16_t)((vdp.address & 0x3F00) | value);
        vdp.second_write = true;
        return;
    }
    vdp.second_write = false;

    switch (value & 0xC0) {
    case 0x00:   // set up for VRAM read
        vdp.address = (uint16_t)(((value & 0x3F) << 8) | vdp.first_byte);
        vdp.read_latch = vdp.vram[vdp.address & 0x3FFF];
        vdp.address = (uint16_t)((vdp.address + 1) & 0x3FFF);
        break;
    case 0x40:   // set up for VRAM write
        vdp.address = (uint16_t)(((value & 0x3F) << 8) | vdp.first_byte);
        break;
    default: {   // register write
        int r = value & 0x07;
        vdp.reg[r] = vdp.first_byte;
        if (r == 1) {
            // Enabling interrupts while the flag is already set fires straight
            // away -- several games depend on this at start-up.
            vdp.irq = irq_enabled() && (vdp.status & 0x80);
        }
        break; }
    }
}

// ---------------------------------------------------------------------------
// Sprite rendering (shared by graphics modes 1, 2 and 3)
// ---------------------------------------------------------------------------
static void HOT(render_sprites)(int y, uint8_t *line) {
    const uint16_t attr_base = (uint16_t)((R5 & 0x7F) << 7);
    const uint16_t patt_base = (uint16_t)((R6 & 0x07) << 11);
    const bool     mag       = (R1 & 0x01) != 0;
    const bool     size16    = (R1 & 0x02) != 0;
    const int      sprite_h  = (size16 ? 16 : 8) * (mag ? 2 : 1);

    int drawn = 0;

    // Marks every x where a sprite pattern bit has already been placed on this
    // line. Serves two purposes: sprite coincidence detection, and priority --
    // an occupied pixel belongs to a lower-numbered, higher-priority sprite and
    // must not be overwritten.
    uint8_t occupied[VDP_ACTIVE_W];
    memset(occupied, 0, sizeof(occupied));

    for (int s = 0; s < 32; s++) {
        const uint16_t a = (uint16_t)(attr_base + s * 4);
        int sy = vdp.vram[a];

        if (sy == 208) break;                 // terminator

        // Y is stored one line early, so a sprite with Y=0 starts on line 1.
        // Values above the terminator are negative, letting a sprite be
        // partially scrolled off the top of the screen. The threshold is 208,
        // not 224: 209..223 are equally negative, and treating them as large
        // positives put those sprites off the bottom instead of clipping them
        // at the top.
        if (sy > 208) sy -= 256;
        sy += 1;
        if (y < sy || y >= sy + sprite_h) continue;

        if (++drawn > 4) {
            if (!(vdp.status & 0x40)) {
                vdp.status = (uint8_t)((vdp.status & 0xE0) | 0x40 | s);
            }
            break;
        }

        int      sx    = vdp.vram[a + 1];
        uint8_t  name  = vdp.vram[a + 2];
        uint8_t  attr  = vdp.vram[a + 3];
        uint8_t  color = (uint8_t)(attr & 0x0F);
        if (attr & 0x80) sx -= 32;            // early-clock bit

        int row = y - sy;
        if (mag) row >>= 1;

        uint16_t pat = (uint16_t)(patt_base + (size16 ? (name & 0xFC) : name) * 8 + row);
        uint8_t  b0  = vdp.vram[pat & 0x3FFF];
        uint8_t  b1  = size16 ? vdp.vram[(pat + 16) & 0x3FFF] : 0;

        const int width = (size16 ? 16 : 8) * (mag ? 2 : 1);
        for (int px = 0; px < width; px++) {
            int bit = mag ? (px >> 1) : px;
            uint8_t on = (bit < 8) ? ((b0 >> (7 - bit)) & 1)
                                   : ((b1 >> (15 - bit)) & 1);
            if (!on) continue;

            int x = sx + px;
            if (x < 0 || x >= VDP_ACTIVE_W) continue;

            if (occupied[x]) {
                // Two sprite pattern bits on the same pixel: set the
                // coincidence flag. This happens regardless of colour, and
                // regardless of which sprite ends up visible.
                vdp.status |= 0x20;

                // Do NOT draw. The pixel already belongs to a lower-numbered
                // sprite, which outranks this one. Overwriting here is what
                // inverted sprite priority.
                continue;
            }
            occupied[x] = 1;

            // Colour 0 is transparent: the pixel still counts as occupied for
            // priority and coincidence, but nothing is drawn, so the
            // background shows through and lower-priority sprites stay hidden
            // behind it -- which is what the hardware does.
            if (color) line[x] = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Background modes
// ---------------------------------------------------------------------------
static void HOT(render_mode_graphics1)(int y, uint8_t *line) {
    const uint16_t name_base  = (uint16_t)((R2 & 0x0F) << 10);
    const uint16_t colr_base  = (uint16_t)(R3 << 6);
    const uint16_t patt_base  = (uint16_t)((R4 & 0x07) << 11);
    const int      row        = y >> 3;
    const int      sub        = y & 7;

    for (int col = 0; col < 32; col++) {
        uint8_t name = vdp.vram[(name_base + row * 32 + col) & 0x3FFF];
        uint8_t pat  = vdp.vram[(patt_base + name * 8 + sub) & 0x3FFF];
        uint8_t clr  = vdp.vram[(colr_base + (name >> 3)) & 0x3FFF];
        uint8_t fg   = (uint8_t)(clr >> 4);
        uint8_t bg   = (uint8_t)(clr & 0x0F);
        uint8_t *p   = line + col * 8;
        for (int i = 0; i < 8; i++)
            p[i] = ((pat >> (7 - i)) & 1) ? fg : bg;
    }
}

static void HOT(render_mode_graphics2)(int y, uint8_t *line) {
    const uint16_t name_base = (uint16_t)((R2 & 0x0F) << 10);
    const int      row       = y >> 3;
    const int      sub       = y & 7;
    const int      third     = row >> 3;      // 0, 1 or 2

    // In Graphics II the pattern/colour tables are split into three 2 KB
    // banks; R4/R3 bits act as masks rather than plain base addresses.
    const uint16_t patt_base = (uint16_t)((R4 & 0x04) << 11);
    const uint16_t colr_base = (uint16_t)((R3 & 0x80) << 6);
    const uint16_t patt_mask = (uint16_t)(((R4 & 0x03) << 8) | 0xFF);
    const uint16_t colr_mask = (uint16_t)(((R3 & 0x7F) << 3) | 0x07);

    for (int col = 0; col < 32; col++) {
        uint8_t  name = vdp.vram[(name_base + row * 32 + col) & 0x3FFF];
        uint16_t idx  = (uint16_t)((third << 8) | name);

        uint16_t pa = (uint16_t)(patt_base + ((idx & patt_mask) * 8) + sub);
        uint16_t ca = (uint16_t)(colr_base + ((idx & colr_mask) * 8) + sub);

        uint8_t pat = vdp.vram[pa & 0x3FFF];
        uint8_t clr = vdp.vram[ca & 0x3FFF];
        uint8_t fg  = (uint8_t)(clr >> 4);
        uint8_t bg  = (uint8_t)(clr & 0x0F);
        uint8_t *p  = line + col * 8;
        for (int i = 0; i < 8; i++)
            p[i] = ((pat >> (7 - i)) & 1) ? fg : bg;
    }
}

static void HOT(render_mode_multicolour)(int y, uint8_t *line) {
    const uint16_t name_base = (uint16_t)((R2 & 0x0F) << 10);
    const uint16_t patt_base = (uint16_t)((R4 & 0x07) << 11);
    const int      row       = y >> 3;
    const int      sub       = ((y >> 2) & 1) + ((row & 3) << 1);

    for (int col = 0; col < 32; col++) {
        uint8_t name = vdp.vram[(name_base + row * 32 + col) & 0x3FFF];
        uint8_t v    = vdp.vram[(patt_base + name * 8 + sub) & 0x3FFF];
        uint8_t left = (uint8_t)(v >> 4);
        uint8_t right= (uint8_t)(v & 0x0F);
        uint8_t *p   = line + col * 8;
        for (int i = 0; i < 4; i++) p[i] = left;
        for (int i = 4; i < 8; i++) p[i] = right;
    }
}

static void HOT(render_mode_text)(int y, uint8_t *line) {
    const uint16_t name_base = (uint16_t)((R2 & 0x0F) << 10);
    const uint16_t patt_base = (uint16_t)((R4 & 0x07) << 11);
    const int      row       = y / 8;
    const int      sub       = y & 7;
    const uint8_t  fg        = (uint8_t)(R7 >> 4);
    const uint8_t  bg        = (uint8_t)(R7 & 0x0F);

    // Text mode is 40 columns of 6 pixels, centred with an 8 pixel border.
    for (int i = 0; i < 8; i++) line[i] = bg;
    for (int i = 248; i < 256; i++) line[i] = bg;

    for (int col = 0; col < 40; col++) {
        uint8_t name = vdp.vram[(name_base + row * 40 + col) & 0x3FFF];
        uint8_t pat  = vdp.vram[(patt_base + name * 8 + sub) & 0x3FFF];
        uint8_t *p   = line + 8 + col * 6;
        for (int i = 0; i < 6; i++)
            p[i] = ((pat >> (7 - i)) & 1) ? fg : bg;
    }
}

// ---------------------------------------------------------------------------
// One scanline
// ---------------------------------------------------------------------------
bool HOT(vdp_scanline)(uint16_t *dest) {
    const int y = vdp.line;
    bool frame_irq = false;

    if (y < VDP_ACTIVE_H && dest) {
        const uint8_t backdrop = (uint8_t)(R7 & 0x0F);
        uint8_t *line = vdp.linebuf;

        if (!screen_on()) {
            memset(line, backdrop, VDP_ACTIVE_W);
        } else {
            switch (mode()) {
            case 0: render_mode_graphics1(y, line);   render_sprites(y, line); break;
            case 1: render_mode_graphics2(y, line);   render_sprites(y, line); break;
            case 2: render_mode_multicolour(y, line); render_sprites(y, line); break;
            case 4: render_mode_text(y, line);        break;   // no sprites in text
            default: memset(line, backdrop, VDP_ACTIVE_W);     break;
            }
        }

        // Expand colour indices to RGB565; index 0 shows the backdrop colour.
        const uint16_t bd = pal565[backdrop ? backdrop : 1];
        for (int x = 0; x < VDP_ACTIVE_W; x++) {
            uint8_t c = line[x];
            dest[x] = c ? pal565[c] : bd;
        }
    }

    if (y == VDP_ACTIVE_H) {
        vdp.status |= 0x80;                    // frame flag
        if (irq_enabled()) vdp.irq = true;
        frame_irq = true;
    }

    if (++vdp.line >= VDP_LINES_NTSC) vdp.line = 0;
    return frame_irq;
}
