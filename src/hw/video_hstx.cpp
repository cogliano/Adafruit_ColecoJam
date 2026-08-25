// video_hstx.cpp -- 640x480p60 DVI output via the RP2350 HSTX peripheral
//
// Approach is the same as the pico-examples hstx/dvi_out_hstx_encoder demo and
// the HSTX driver in fhoedemakers' pico-snesPlus / pico-infonesPlus: a chained
// DMA walks a list of "command" words that alternately push raw TMDS sync
// symbols (for blanking) and stream framebuffer pixels through the HSTX
// expander (for active video).
//
// We keep a 320x240 RGB565 framebuffer and emit every line twice with each
// pixel repeated twice, giving a 2x integer scale to 640x480. No filtering,
// no fractional scaling -- retro output looks best sharp.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_hstx.h"
#include "config.h"

// Built only when this is the selected driver; video_hdmi.cpp provides the
// same interface on top of pico_hdmi otherwise.
#if VIDEO_DRIVER == VIDEO_DRIVER_HSTX

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/clocks.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Framebuffer -- 320x240 RGB565, 150 KB. Lives in SRAM so the DMA never has to
// contend with XIP.
// ---------------------------------------------------------------------------
static uint16_t __attribute__((aligned(4))) framebuffer[FB_WIDTH * FB_HEIGHT];

uint16_t *video_framebuffer(void) { return framebuffer; }

// ---------------------------------------------------------------------------
// 640x480p60 timing (VESA / CEA-861 mode 1)
//   pixel clock 25.175 MHz, we run 25.2 MHz which is inside tolerance
// ---------------------------------------------------------------------------
#define MODE_H_SYNC_POLARITY   0
#define MODE_H_FRONT_PORCH     16
#define MODE_H_SYNC_WIDTH      96
#define MODE_H_BACK_PORCH      48
#define MODE_H_ACTIVE_PIXELS   640

#define MODE_V_SYNC_POLARITY   0
#define MODE_V_FRONT_PORCH     10
#define MODE_V_SYNC_WIDTH      2
#define MODE_V_BACK_PORCH      33
#define MODE_V_ACTIVE_LINES    480

#define MODE_H_TOTAL (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
                      MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
                      MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// TMDS control symbols. Lane 0 carries the sync signals on its two control
// bits (CTL0 = HSYNC, CTL1 = VSYNC); lanes 1 and 2 sit at control 00 the whole
// time. Each word packs all three lanes into 30 bits, 10 per lane.
//
// These must be single parenthesised expressions, not comma lists: the
// preprocessor counts macro arguments before expanding them, so PACK3(SYNC_x)
// would be seen as one argument to a three-argument macro.
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// HSTX command words
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ---------------------------------------------------------------------------
// Command lists for vblank and active lines
// ---------------------------------------------------------------------------
static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS,
};

// ---------------------------------------------------------------------------
// DMA state
// ---------------------------------------------------------------------------
// The two command/data DMA channels. These MUST be claimed from the SDK's
// allocator rather than hardcoded: audio_init() and Pico-PIO-USB both call
// dma_claim_unused_channel(), and if video has silently squatted on channels
// 0 and 1 without claiming them, the audio driver is handed those same two
// channels and reprograms them for I2S -- tearing down the DVI chain moments
// after it starts. The symptom is a monitor that reports no signal while every
// other part of the boot sequence completes normally.
static int DMACH_PING = -1;
static int DMACH_PONG = -1;

static bool     dma_pong = false;
static uint     v_scanline = 2;
static bool     vactive_cmdlist_posted = false;
static volatile uint32_t frame_counter = 0;

// Horizontal 2x scaling. The HSTX expander unpacks several pixels from each
// 32-bit word, but it CANNOT duplicate a pixel -- there is no such mode. So a
// 320-pixel framebuffer row has to be widened to 640 pixels before the DMA
// sees it. We do that by packing each pixel into both halves of a word:
//
//     word[i] = (px << 16) | px
//
// and telling the expander to take 2 pixels per word, 16 bits apart. That
// gives 320 words -> 640 pixels, which matches the TMDS command count exactly.
// Vertical 2x is free: consecutive output lines read the same framebuffer row.
//
// Two buffers, alternating per line, so the line being built is never the one
// the DMA is currently reading. 1280 bytes each.
static uint32_t line_buf[2][FB_WIDTH] __attribute__((aligned(4)));
static uint      line_buf_idx = 0;

static inline void __scratch_x("hstx") expand_line(const uint16_t *src,
                                                   uint32_t *dst) {
    for (uint i = 0; i < FB_WIDTH; i++) {
        uint32_t px = src[i];
        dst[i] = (px << 16) | px;
    }
}
static void __scratch_x("hstx") dma_irq_handler(void) {
    // Ping/pong: whichever channel just finished gets reprogrammed for the
    // line after next.
    uint ch_num = (uint)(dma_pong ? DMACH_PONG : DMACH_PING);
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH &&
        v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < (MODE_V_TOTAL - MODE_V_ACTIVE_LINES)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;

        // Prime the very first line of the frame. Every other line is prepared
        // one line ahead (below), but line 0 has no predecessor to hide behind,
        // so do it here -- during the last blanking line, which is idle.
        uint out_line = v_scanline - (MODE_V_TOTAL - MODE_V_ACTIVE_LINES);
        if (out_line == 0)
            expand_line(&framebuffer[0], line_buf[line_buf_idx]);
    } else {
        // Source one framebuffer row, widened 2x into a line buffer. Two
        // consecutive output lines share the same row, which is the vertical
        // half of the 2x scale.
        uint out_line = v_scanline - (MODE_V_TOTAL - MODE_V_ACTIVE_LINES);
        uint fb_row   = out_line >> 1;

        (void)fb_row;

        // Hand the DMA the line that was prepared during the previous line's
        // transfer, then start preparing the next one.
        ch->read_addr = (uintptr_t)line_buf[line_buf_idx];
        ch->transfer_count = FB_WIDTH;      // 320 words -> 640 pixels
        vactive_cmdlist_posted = false;

        // Timing, and why this is here rather than just above the read_addr:
        // expand_line takes ~6.35 us. The gap between programming the pixel
        // transfer and the DMA needing the data is only the horizontal blanking
        // period, also ~6.35 us -- no margin at all, so any contention stalls
        // the line and the picture shears.
        //
        // Doing it AFTER the transfer is programmed means it overlaps the
        // 25.4 us pixel transfer instead: 4x the headroom, into a buffer the
        // DMA is not touching.
        uint next_line = out_line + 1;
        if (next_line < MODE_V_ACTIVE_LINES) {
            line_buf_idx ^= 1;
            expand_line(&framebuffer[(next_line >> 1) * FB_WIDTH],
                        line_buf[line_buf_idx]);
        }
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL;
        if (v_scanline == 0) frame_counter++;
    }
}

uint32_t video_frames_rendered(void) { return frame_counter; }

// Set once video_init() has actually brought the DMA chain up. With
// ENABLE_VIDEO 0 this stays false and vsync waits become a plain delay, so the
// emulator loop still paces itself instead of spinning on a counter that will
// never advance.
static volatile bool video_active = false;

void video_wait_vsync(void) {
    if (!video_active) { sleep_ms(16); return; }
    uint32_t f = frame_counter;
    while (frame_counter == f) tight_loop_contents();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void video_init(void) {
    memset(framebuffer, 0, sizeof(framebuffer));

    // Give DMA priority on the SRAM bus so video never tears under CPU load.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    // clk_hstx comes straight off pll_usb, which setup_clocks() has retuned to
    // 126 MHz. Divide by 1. clk_sys is 240 MHz for Pico-PIO-USB's benefit and
    // is deliberately not involved here -- 240 cannot produce 126.
    clock_configure(clk_hstx,
                    0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    HSTX_CLK_HZ,
                    HSTX_CLK_HZ);

    // --- TMDS encoder: RGB565 in, 3 lanes out --------------------------
    // Lane 2 (red)   : 5 bits at bit 11 -> rotate left 29
    // Lane 1 (green) : 6 bits at bit 5  -> rotate left 3, take 5
    // Lane 0 (blue)  : 5 bits at bit 0  -> rotate left 8
    hstx_ctrl_hw->expand_tmds =
        4u  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        29u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5u  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        3u  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4u  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        8u  << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Two RGB565 pixels per 32-bit word, 16 bits apart. 320 words per line
    // therefore produce exactly the 640 pixels the active-line TMDS command
    // asks for. Getting this count wrong desynchronises the command stream and
    // the monitor sees no valid signal at all.
    hstx_ctrl_hw->expand_shift =
        2u  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB    |
        1u  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0u  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serialiser: 10 TMDS bits per pixel emitted as 5 shifts of 2 bits (DDR),
    // and the generated pixel clock is clk_hstx / 5.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB   |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB    |
        HSTX_CTRL_CSR_EN_BITS;

    // --- Pin mapping ----------------------------------------------------
    // Fruit Jam wiring: CK-/CK+ on 12/13, D0-/D0+ on 14/15, D1 on 16/17,
    // D2 on 18/19. The N pin of each pair is the inverted output.
    static const struct { uint gpio; uint sel_p; bool invert; } lanes[] = {
        { PIN_HSTX_CK_N, 0, true  }, { PIN_HSTX_CK_P, 0, false },
        { PIN_HSTX_D0_N, 1, true  }, { PIN_HSTX_D0_P, 1, false },
        { PIN_HSTX_D1_N, 2, true  }, { PIN_HSTX_D1_P, 2, false },
        { PIN_HSTX_D2_N, 3, true  }, { PIN_HSTX_D2_P, 3, false },
    };

    for (uint i = 0; i < count_of(lanes); i++) {
        uint bit = lanes[i].gpio - 12;      // HSTX bit index 0..7
        uint32_t v;
        if (lanes[i].sel_p == 0) {
            // Clock lane: a fixed 1010101010 pattern.
            v = HSTX_CTRL_BIT0_CLK_BITS;
        } else {
            uint lane = lanes[i].sel_p - 1;
            v = (lane * 10)      << HSTX_CTRL_BIT0_SEL_P_LSB |
                (lane * 10 + 1)  << HSTX_CTRL_BIT0_SEL_N_LSB;
        }
        if (lanes[i].invert) v |= HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit] = v;
    }

    for (uint i = 12; i <= 19; i++) {
        gpio_set_function(i, (gpio_function_t)0);   // FUNCSEL 0 = HSTX

        // The pad defaults (4 mA, slow slew) do not give enough edge rate for
        // 252 Mbps TMDS. With weak pads the eye closes far enough that a sink
        // will not lock at all -- the monitor reports no signal rather than a
        // corrupted picture, which makes this look like a configuration fault
        // when it is really an electrical one.
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
    }

    // --- DMA ping-pong --------------------------------------------------
    // Claim before configuring, so nothing else can be handed these channels.
    DMACH_PING = dma_claim_unused_channel(true);
    DMACH_PONG = dma_claim_unused_channel(true);

    dma_channel_config c;

    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    // Highest priority. The HSTX FIFO is shallow: if this handler is delayed
    // by even a few microseconds the command stream stalls mid-line and the
    // picture shears or rolls. Running at 0x80 produced exactly that -- a
    // picture that locked at 640x480 but was unstable and hard to read.
    //
    // This is safe now only because usb_msc_init() and usb_host_init() both
    // run BEFORE video_init(). PIO-USB's setup is what could not tolerate a
    // top-priority video interrupt; by the time we get here it is finished.
    irq_set_priority(DMA_IRQ_0, 0x00);

    dma_channel_start(DMACH_PING);
    video_active = true;
}

// ---------------------------------------------------------------------------
// Drawing helpers used by the menu and the emulator
// ---------------------------------------------------------------------------
void video_clear(uint16_t colour) {
    uint32_t pair = ((uint32_t)colour << 16) | colour;
    uint32_t *p = (uint32_t *)framebuffer;
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT / 2; i++) p[i] = pair;
}

void video_fill_rect(int x, int y, int w, int h, uint16_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint16_t *p = &framebuffer[(y + r) * FB_WIDTH + x];
        for (int cx = 0; cx < w; cx++) p[cx] = colour;
    }
}

void video_set_pixel(int x, int y, uint16_t colour) {
    if ((unsigned)x < FB_WIDTH && (unsigned)y < FB_HEIGHT)
        framebuffer[y * FB_WIDTH + x] = colour;
}

#endif  // VIDEO_DRIVER == VIDEO_DRIVER_HSTX
