// video_hdmi.cpp -- video and audio over HDMI, using the pico_hdmi library
//
//   https://github.com/fliperama86/pico_hdmi
//
// Selected by VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI in config.h. Presents the
// same interface as the built-in HSTX driver in video_hstx.cpp -- video_init,
// video_framebuffer, video_wait_vsync and the drawing helpers -- so nothing
// above this file changes.
//
// Why use the library rather than extend our own HSTX driver: carrying sound
// over HDMI needs TERC4 symbol encoding, data island periods with guard bands
// inserted into blanking, BCH error correction on every packet, AVI and Audio
// InfoFrames, and N/CTS clock regeneration. pico_hdmi already implements all of
// that and is tested on this class of board.
//
// Two structural differences from video_hstx.cpp:
//
//   * The library owns CORE 1. video_output_core1_run() never returns, so USB
//     host and the emulator both live on core 0 here. That is the reverse of
//     the built-in driver's arrangement, and it is fine because core 0 no
//     longer carries a video interrupt for PIO-USB to contend with.
//
//   * clk_hstx is derived by the library as clk_sys / PICO_HDMI_HSTX_CLK_DIV,
//     an integer divide. clk_sys must therefore be 126 MHz x that divider.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#if VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI

#include "video_hstx.h"          // the interface we implement
#include "hdmi_audio.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>

extern "C" {
#include "pico_hdmi/video_output.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
}

// The library exports MODE_HSTX_CLK_DIV publicly, so the clock relationship can
// be checked at compile time rather than discovered as a blank screen.
//
// clk_hstx = clk_sys / MODE_HSTX_CLK_DIV, and 640x480p60 needs 126 MHz. If the
// CMake cache variable PICO_HDMI_HSTX_CLK_DIV is not set before
// add_subdirectory(), this defaults to 1 and HSTX is driven at double spec.
// pico_hdmi claims DMA channels 0 and 1 by name inside video_output_init().
// Anything else that reserves one of them first makes that call panic, which
// surfaces as a hard fault with no other clue.
static_assert(PIO_USB_TX_DMA_CH != 0 && PIO_USB_TX_DMA_CH != 1,
              "pico_hdmi hardcodes DMA channels 0 and 1; PIO-USB must use "
              "another. See PIO_USB_TX_DMA_CH in config.h.");

static_assert((CV_SYS_CLK_KHZ * 1000ull) / MODE_HSTX_CLK_DIV == HSTX_CLK_HZ,
              "clk_sys / MODE_HSTX_CLK_DIV must equal 126 MHz. Set "
              "PICO_HDMI_HSTX_CLK_DIV in CMakeLists.txt before "
              "add_subdirectory(pico_hdmi).");

// ---------------------------------------------------------------------------
// Framebuffer -- 320x240 RGB565, same as the built-in driver so the emulator,
// the menu and the drawing helpers are unchanged.
// ---------------------------------------------------------------------------
static uint16_t __attribute__((aligned(4))) framebuffer[FB_WIDTH * FB_HEIGHT];

// The library does NOT scale. It reads MODE_H_ACTIVE_PIXELS / 2 = 320 words
// per active line -- a full 640-pixel output line -- and frame_width/height
// passed to video_output_init() are merely recorded, not acted on.
//
// So each 320-pixel framebuffer row has to be widened to 640 here. Handing
// back a 160-word row instead made the library read on into the following row,
// putting two rows side by side, while active_line running 0..479 over a
// 240-row buffer stacked two copies vertically: the 2x2 grid.
//
// Two buffers, alternating, so the line being built is never the one the DMA
// is reading.
static uint32_t line_buf[2][FB_WIDTH] __attribute__((aligned(4)));
static uint     line_idx = 0;

static volatile uint32_t frame_counter = 0;
static volatile bool     video_active  = false;

uint16_t *video_framebuffer(void) { return framebuffer; }
uint32_t  video_frames_rendered(void) { return frame_counter; }

// ---------------------------------------------------------------------------
// Scanline supply
// ---------------------------------------------------------------------------
// Returning a pointer rather than filling a caller-provided buffer keeps the
// per-line work to the horizontal expansion alone. The library's scanline
// budget is roughly 6 us, and widening 320 pixels costs well under half of it.
static const uint32_t *scanline_cb(uint32_t v_scanline, uint32_t active_line) {
    (void)v_scanline;

    // Two output lines per framebuffer row: the vertical half of the 2x scale.
    const uint fb_row = (active_line >> 1) % FB_HEIGHT;
    const uint16_t *src = &framebuffer[fb_row * FB_WIDTH];

    line_idx ^= 1;
    uint32_t *dst = line_buf[line_idx];

    // Each pixel written into both halves of a word: 320 words out, 640 pixels
    // on the wire. The horizontal half of the 2x scale.
    for (uint i = 0; i < FB_WIDTH; i++) {
        const uint32_t px = src[i];
        dst[i] = (px << 16) | px;
    }
    return dst;
}

static void vsync_cb(void) { frame_counter++; }

// ---------------------------------------------------------------------------
// HDMI audio
// ---------------------------------------------------------------------------
// Samples are handed to the library four at a time, each wrapped in an audio
// sample packet and pre-encoded into a data island. The queue is topped up from
// the emulator loop; if it runs dry the library sends silence packets rather
// than glitching, and hstx_di_queue_silence_count records that.
static int audio_frame_counter = 0;

void hdmi_audio_init(void) {
    hstx_di_queue_init();
    hstx_di_queue_set_sample_rate(AUDIO_SAMPLE_RATE);
}

uint32_t hdmi_audio_queue_level(void) { return hstx_di_queue_get_level(); }

uint32_t hdmi_audio_underruns(void) { return hstx_di_queue_silence_count; }

// Returns how many samples were consumed; may be fewer than offered when the
// queue is full, in which case the caller should keep the remainder.
int hdmi_audio_submit(const int16_t *mono, int count) {
    int used = 0;
    while (used + 4 <= count &&
           hstx_di_queue_get_level() < HDMI_AUDIO_QUEUE_TARGET) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            // The PSG is mono; feed the same sample to both channels.
            samples[i].left  = mono[used + i];
            samples[i].right = mono[used + i];
        }
        used += 4;

        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples_cs_rate(
            &packet, samples, 4, audio_frame_counter, AUDIO_SAMPLE_RATE);

        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false,
                                hstx_di_queue_get_hsync_active());
        if (!hstx_di_queue_push(&island)) break;   // full; stop early
    }
    return used;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
// Stage markers. There is no display yet and no serial by default, so the LED
// is the only channel. Each stage blinks BEFORE the call it names, so the last
// count seen identifies the call that failed:
//
//   1  entering video_init
//   2  hstx_di_queue_init
//   3  video_output_init
//   4  scanline / vsync callbacks
//   5  dvi mode + audio sample rate
//   6  multicore_launch_core1 -- hands core 1 to the library
//
// Reaching main's own blink(3) afterwards means all six returned.
static void stage(int n) {
#if !BOOT_DIAGNOSTICS
    (void)n;
    return;
#endif
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    for (int i = 0; i < n; i++) {
        gpio_put(PIN_LED, 0); sleep_ms(120);
        gpio_put(PIN_LED, 1); sleep_ms(120);
    }
    sleep_ms(500);
}

void video_init(void) {
    stage(1);
    memset(framebuffer, 0, sizeof(framebuffer));
    memset(line_buf, 0, sizeof(line_buf));

    stage(2);
    hdmi_audio_init();

    stage(3);
    // These are only recorded by the library; nothing reads them on the
    // scanline-pointer path. Pass the output geometry, which is what the
    // callback actually has to supply.
    video_output_init(DVI_WIDTH, DVI_HEIGHT);

    stage(4);
    video_output_set_scanline_pointer_callback(scanline_cb);
    video_output_set_vsync_callback(vsync_cb);

    stage(5);
    // false = HDMI mode with data islands, which is what carries the audio.
    video_output_set_dvi_mode(AUDIO_SINK != AUDIO_SINK_HDMI);
    pico_hdmi_set_audio_sample_rate(AUDIO_SAMPLE_RATE);

    stage(6);
    // Core 1 belongs to the library from here on; its loop never returns.
    multicore_launch_core1(video_output_core1_run);

    video_active = true;
}

void video_wait_vsync(void) {
    if (!video_active) { sleep_ms(16); return; }
    const uint32_t f = frame_counter;
    while (frame_counter == f) tight_loop_contents();
}

// ---------------------------------------------------------------------------
// Drawing helpers -- identical to the built-in driver
// ---------------------------------------------------------------------------
void video_clear(uint16_t colour) {
    const uint32_t pair = ((uint32_t)colour << 16) | colour;
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

#endif  // VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
