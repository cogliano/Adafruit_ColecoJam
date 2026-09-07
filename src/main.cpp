// main.cpp -- Adafruit_ColecoJam entry point
//
// Boot sequence
//   1. Clock to 252 MHz, bring up HSTX DVI, show a splash.
//   2. Start USB device (MSC) and USB host (gamepads).
//   3. Mount the SD card, load coleco/COLECO.BIN.
//   4. If the cartridge reader shield has a cartridge seated, dump it and run
//      it directly -- otherwise show the ROM browser.
//   5. Run the machine.
//
// Core split
//   core0: emulation (Z80 + VDP + PSG) and USB polling
//   core1: audio mixing and I2S buffer refills
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "emu/coleco.h"
#include "emu/tms9918.h"
#include "emu/sn76489.h"
#include "emu/z80.h"
#include "hw/video_hstx.h"
#include "hw/audio.h"
#include "hw/sdcard.h"
#include "hw/usb_host.h"
#include "hw/usb_msc.h"
#include "hw/cart_reader.h"
#include "hw/hdmi_audio.h"
#include "ui/menu.h"

// Character row for the in-game debug overlay: below the 256x192 picture,
// which ends at row 26 of the 30-row 320x240 framebuffer.
#define ROWS_DEBUG 28

extern "C" {
#include "ff.h"
}

// ---------------------------------------------------------------------------
// Storage. The BIOS and cartridge both live in SRAM for full-speed access; a
// 32 KB cartridge plus 8 KB BIOS plus 16 KB VRAM plus the 150 KB framebuffer
// fits comfortably in the RP2350B's 520 KB.
// ---------------------------------------------------------------------------
static uint8_t bios_rom[CV_BIOS_SIZE];
static uint8_t cart_rom[CV_ROM_MAX];
static uint32_t cart_len = 0;

static FATFS fs;

// ---------------------------------------------------------------------------
// Core 1: audio
// ---------------------------------------------------------------------------
#if ENABLE_AUDIO && AUDIO_SINK == AUDIO_SINK_CODEC && \
    VIDEO_DRIVER != VIDEO_DRIVER_PICO_HDMI
static int16_t mix_buf[AUDIO_BUF_SAMPLES];
static volatile bool audio_running = false;
#endif
static volatile bool host_ready    = false;

// Core 1 owns the USB host stack and audio.
//
// PIO-USB must live here rather than on core 0. It bit-bangs USB in PIO and
// needs prompt interrupt service, but core 0 runs the video DMA interrupt at
// priority 0, which blocks for ~6.35 us twice per scanline. That is long
// enough to desync PIO-USB. Interrupts are per-core on RP2350, so running
// tuh_init() here registers its handler on core 1, out of video's way.
//
// tuh_init() must be called on the same core that will call tuh_task().
#if VIDEO_DRIVER != VIDEO_DRIVER_PICO_HDMI
// With VIDEO_DRIVER_PICO_HDMI this function is never launched: the library's
// own loop takes core 1 and never returns, so USB host and audio run on core 0
// instead. That is safe there precisely because core 0 no longer carries a
// video interrupt for PIO-USB to contend with -- the reason the host was moved
// off core 0 in the first place no longer applies.
static void core1_main(void) {
#if USB_HOST_ON_CORE1
    usb_host_init();
#endif
    host_ready = true;

#if ENABLE_AUDIO && AUDIO_SINK == AUDIO_SINK_CODEC
    audio_init();
    audio_set_volume(80);
    audio_running = true;
#endif

    for (;;) {
#if USB_HOST_ON_CORE1
        usb_host_task();
#endif
#if ENABLE_AUDIO && AUDIO_CPU_FEED
        // No DMA: generate a block and push it straight into the SM.
        {
#if AUDIO_TEST_TONE
            static uint32_t phase = 0;
            const uint32_t period = AUDIO_SAMPLE_RATE / 440;
            for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
                mix_buf[i] = (phase < period / 2) ? 8000 : -8000;
                if (++phase >= period) phase = 0;
            }
#else
            psg_render(mix_buf, AUDIO_BUF_SAMPLES);
#endif
            audio_push_blocking(mix_buf, AUDIO_BUF_SAMPLES);
        }
#elif ENABLE_AUDIO
        int half = audio_buffer_needed();
        if (half >= 0) {
#if AUDIO_TEST_TONE
            // 440 Hz square wave, a quarter of full scale. Exercises the I2S
            // path, codec and amplifier without involving the emulator.
            static uint32_t phase = 0;
            const uint32_t period = AUDIO_SAMPLE_RATE / 440;
            for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
                mix_buf[i] = (phase < period / 2) ? 8000 : -8000;
                if (++phase >= period) phase = 0;
            }
#else
            psg_render(mix_buf, AUDIO_BUF_SAMPLES);
#endif
            audio_submit(half, mix_buf, AUDIO_BUF_SAMPLES);
        }
#else
        tight_loop_contents();
#endif
    }
}
#endif  // VIDEO_DRIVER != VIDEO_DRIVER_PICO_HDMI

// Retune the clock tree so PIO-USB, DVI and the native USB device can all have
// the frequency each requires. See the comment block in config.h.
//
// Ordering is critical: clk_usb boots sourced from pll_usb, so it must be
// moved onto pll_sys BEFORE pll_usb is retuned, or the USB device clock
// collapses mid-reconfiguration.
static void setup_clocks(void) {
#if VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
    // Nothing to do. pico_hdmi derives clk_hstx from clk_sys itself, and
    // clk_usb stays on pll_usb at its stock 48 MHz.
    return;
#elif USB_CLOCK_TEST_MODE
    // Stock clock tree: clk_usb stays on pll_usb at 48 MHz, pll_usb is not
    // retuned, and clk_hstx is never configured. Nothing to do.
    return;
#else
    // 1. clk_usb: pll_usb (48 MHz) -> pll_sys / 5 = 48 MHz. Same frequency,
    //    different source.
    clock_configure(clk_usb,
                    0,
                    CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    CV_SYS_CLK_KHZ * 1000u,
                    48000000);

    // 2. clk_adc also defaults to pll_usb. Unused here, but repoint it so it
    //    does not quietly end up at 126 MHz.
    clock_configure(clk_adc,
                    0,
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    CV_SYS_CLK_KHZ * 1000u,
                    48000000);

    // 3. pll_usb now feeds nothing, so it is free to become the HSTX clock.
    pll_init(pll_usb, 1, CV_PLL_USB_VCO_HZ,
             CV_PLL_USB_POSTDIV1, CV_PLL_USB_POSTDIV2);
#endif
}

// Hard fault / lockup handler. Blinks very fast and forever -- unmistakably
// different from the slow boot codes and the slow fatal codes. If you see
// this, the CPU faulted; if the LED simply stops, it is a hang or a deadlock,
// not a fault. The two need completely different investigation, and without
// this they look identical.
extern "C" void isr_hardfault(void) {
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    // THREE slow blinks, then a long pause, forever.
    //
    // The previous version used a tight nop loop that ran at 40-100 Hz -- far
    // too fast to see, so a hard fault looked exactly like the LED being stuck
    // on. That is a bad failure signature for the one condition that most
    // needs to be obvious. These counts give roughly a quarter-second per
    // half-blink at 252 MHz; the exact rate does not matter, only that it is
    // slow enough to count.
    for (;;) {
        for (int b = 0; b < 3; b++) {
            gpio_put(PIN_LED, 0);
            for (volatile uint32_t i = 0; i < 12000000u; i++) __asm volatile("nop");
            gpio_put(PIN_LED, 1);
            for (volatile uint32_t i = 0; i < 12000000u; i++) __asm volatile("nop");
        }
        for (volatile uint32_t i = 0; i < 60000000u; i++) __asm volatile("nop");
    }
}

// Watch for Button 1 during a short window at boot. Holding it enables the
// USB mass-storage device, so the SD card appears as a drive on a host.
//
// The LED stays lit for the whole window so there is something to press
// against, and returns as soon as the button is seen -- boot is only delayed
// by the full window when nothing is pressed.
//
// Note Button 1 is also BOOTSEL. Holding it *through reset* enters the RP2350
// bootloader instead, which is a different thing entirely: press it after the
// board starts, not before.
static bool wait_for_msc_request(void) {
#if !MSC_REQUIRES_BUTTON
    return true;
#else
    absolute_time_t deadline = make_timeout_time_ms(MSC_BUTTON_WINDOW_MS);
    gpio_put(PIN_LED, 0);                   // LED on: window is open
    bool requested = false;
    while (!time_reached(deadline)) {
        if (!gpio_get(PIN_BUTTON1)) { requested = true; break; }
        sleep_ms(10);
    }
    gpio_put(PIN_LED, 1);

    // Wait for release, so the press cannot also trigger the bootloader
    // shortcut in run_emulator() a moment later.
    if (requested) {
        absolute_time_t rel = make_timeout_time_ms(3000);
        while (!gpio_get(PIN_BUTTON1) && !time_reached(rel)) sleep_ms(10);
        sleep_ms(50);                       // debounce
    }
    return requested;
#endif
}

// Wait until every controller button is released, or until a timeout.
//
// The button used to pick a ROM is still held when the emulator starts, and
// the mapping sends A to ColecoVision keypad 1 and START to keypad #. Many
// cartridges read the keypad on their title screen to choose a game mode, so
// launching with A held makes the game start as though 1 had been pressed --
// skipping the menu the player wanted to see.
//
// Draining the press here is much better than having the emulator ignore
// input for a while: real releases are respected immediately, and a stuck or
// mis-decoded button cannot hang the boot because of the timeout.
static void wait_for_button_release(void) {
    absolute_time_t deadline = make_timeout_time_ms(2000);
    for (;;) {
        usb_msc_task();
#if !USB_HOST_ON_CORE1
        usb_host_task();
#endif
        if ((usb_host_raw_buttons(0) | usb_host_raw_buttons(1)) == 0) return;
        if (time_reached(deadline)) return;
        sleep_ms(5);
    }
}

// Blink the LED n times. The LED is active LOW. Used as a boot progress code
// so a blank monitor can be diagnosed without a serial cable:
//   1 = reached main, clocks configured
//   2 = usb_msc_init() returned  (TinyUSB device stack up)
//   3 = video_init() returned    (or skipped, if ENABLE_VIDEO is 0)
//   4 = core1 up: USB host + audio initialised
//   5 = SD card mounted and BIOS loaded
//
// After that, a repeating group of 1-6 blinks with a long pause is a fatal
// error code -- see fatal() below, not the same as boot progress.
static void blink(int n) {
#if !BOOT_DIAGNOSTICS
    // Progress blinks compiled out. The build ID on the menu's title bar
    // already confirms which firmware is running, and fatal codes below are
    // unaffected.
    (void)n;
    return;
#else
    for (int i = 0; i < n; i++) {
        gpio_put(PIN_LED, 0); sleep_ms(150);
        gpio_put(PIN_LED, 1); sleep_ms(150);
    }
    sleep_ms(600);
#endif
}
static uint32_t load_file(const char *path, uint8_t *dst, uint32_t max_len) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;

    UINT br = 0;
    FRESULT r = f_read(&f, dst, max_len, &br);
    f_close(&f);
    return (r == FR_OK) ? (uint32_t)br : 0;
}

// ---------------------------------------------------------------------------
// Fatal error: show it on screen and stop, but keep USB alive so the user can
// still fix the SD card contents over the drag-and-drop drive.
// ---------------------------------------------------------------------------
// `code` is blinked forever so the failure is identifiable with no display:
//   1 = SD: nothing responded to CMD0 (card absent, unseated, wiring)
//   2 = SD: card answered but failed the CMD8 voltage/pattern check
//   3 = SD: card present and talking but never finished initialising
//   4 = SD: card held the bus busy and never went ready
//   9 = SD: FAT mount failed (not FAT32?)
//
// Boot progress blinks are one-shot and count 1..5; fatal codes repeat forever
// with a long gap. A repeating group is always an error, never progress.
//   5 = coleco/COLECO.BIN missing or shorter than 8 KB
//   6 = emulator init failed
//   7 = no /coleco directory on the card
//   8 = ROM file unreadable
[[noreturn]] static void fatal(int code, const char *line1, const char *line2) {
    menu_message("ERROR", line1, line2, true);
    // Only the MSC device task runs here, deliberately. usb_host_task() is
    // NOT called: the host stack is useless in a fatal state, and a crash
    // inside tuh_task() would take the whole board down -- which stops the
    // video DMA chain being reprogrammed, so the display goes dark too. If
    // this loop now blinks forever, tuh_task() was the crash.
    for (;;) {
        for (int i = 0; i < code; i++) {
            gpio_put(PIN_LED, 0);
            for (int t = 0; t < 15; t++) { usb_msc_task(); sleep_ms(10); }
            gpio_put(PIN_LED, 1);
            for (int t = 0; t < 15; t++) { usb_msc_task(); sleep_ms(10); }
        }
        for (int t = 0; t < 120; t++) {
            usb_msc_task();
            // Require the button held ~200 ms. GPIO0 is also BOOTSEL; a
            // momentary glitch here would reboot into the bootloader, which
            // looks exactly like the board dying mid-blink.
            if (!gpio_get(PIN_BUTTON1)) {
                int held = 0;
                while (!gpio_get(PIN_BUTTON1) && held < 20) { sleep_ms(10); held++; }
                if (held >= 20) reset_usb_boot(0, 0);
            }
            sleep_ms(10);
        }
    }
}

// ---------------------------------------------------------------------------
// Emulator loop
// ---------------------------------------------------------------------------
[[noreturn]] static void run_emulator(void) {
    uint16_t *fb = video_framebuffer();

    // The ColecoVision image is 256x192 centred in the 320x240 framebuffer.
    // Draw the border once; the emulator only ever touches the inner region.
    video_clear(RGB565(0, 0, 0));

    for (;;) {
        // One frame: 262 scanlines of Z80 + VDP.
        for (int y = 0; y < CV_LINES_PER_FRAME; y++) {
            uint16_t *dest = nullptr;
            if (vdp.line < VDP_ACTIVE_H) {
                dest = fb + (size_t)(CV_ORIGIN_Y + vdp.line) * FB_WIDTH + CV_ORIGIN_X;
            }
            cv_run_scanline(dest);
        }

        // Poll USB and refresh controller state once per frame -- the
        // ColecoVision's own scan rate is slower than this anyway.
#if !USB_HOST_ON_CORE1 || VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
        // With pico_hdmi the library owns core 1, so the host stack is
        // serviced here regardless of USB_HOST_ON_CORE1.
        usb_host_task();
#endif
        usb_msc_task();
        usb_host_update_coleco();

#if AUDIO_SINK == AUDIO_SINK_HDMI
        // Top up the HDMI audio queue until it reaches its target, rather than
        // rendering a single fixed block per frame.
        //
        // One 512-sample block per frame supplies only about 70% of what the
        // stream consumes (44100 / 59.92 = 736 samples per frame), so the
        // queue ran dry every frame and the library filled the gap with
        // silence packets. That is what the static was.
        //
        // The guard caps the work per frame so a stall here can never starve
        // the emulator; the queue is deep enough to ride out an occasional
        // short fill.
        {
            static int16_t pend_buf[AUDIO_BUF_SAMPLES];
            static int     pend_len = 0;

            for (int guard = 0; guard < 8; guard++) {
                if (hdmi_audio_queue_level() >= HDMI_AUDIO_QUEUE_TARGET) break;

                if (pend_len == 0) {
                    psg_render(pend_buf, AUDIO_BUF_SAMPLES);
                    pend_len = AUDIO_BUF_SAMPLES;
                }

                const int used = hdmi_audio_submit(pend_buf, pend_len);
                if (used <= 0) break;               // queue full or wedged

                if (used < pend_len)
                    memmove(pend_buf, pend_buf + used,
                            (size_t)(pend_len - used) * sizeof(int16_t));
                pend_len -= used;
            }
        }
#endif

        // Button 1 returns to the bootloader so new firmware can be flashed
        // without reaching for the BOOTSEL/reset dance. It must be HELD for a
        // second: the same button now also requests USB drive mode at boot,
        // and a momentary read would turn that press into a surprise reboot.
        if (!gpio_get(PIN_BUTTON1)) {
            int held = 0;
            while (!gpio_get(PIN_BUTTON1) && held < 100) { sleep_ms(10); held++; }
            if (held >= 100) reset_usb_boot(0, 0);
        }

#if EMU_DEBUG_OVERLAY
        // Emulated CPU state, refreshed a few times a second in the border
        // below the picture. When the game locks up this freezes with it, and
        // what it freezes ON says which kind of failure it is:
        //
        //   PC parked in ROM with IFF off and HALT set -> waiting for an
        //     interrupt that never comes: a VDP/NMI problem
        //   PC at 0038 or cycling in a tiny range -> executing 0xFF from
        //     unmapped memory, i.e. it already jumped somewhere wrong
        //   SP outside 6000-7FFF -> the stack has escaped the 1 KB of RAM,
        //     which corrupts everything shortly afterwards
        {
            static uint32_t dbg = 0;
            static uint16_t pc_lo = 0xFFFF, pc_hi = 0x0000;
            if (++dbg >= 20) {
                dbg = 0;
                char l[48];
                snprintf(l, sizeof(l),
                         "PC%04X SP%04X HL%04X(%02X) A%02X %s",
                         z80.pc.w, z80.sp.w, z80.hl.w,
                         cv_bus_read(z80.hl.w), z80.af.b.h,
                         z80.iff1 ? "EI" : "DI");
                menu_debug_line(ROWS_DEBUG, l);

                // Second line: the vertical-blank machinery, plus how far the
                // PC has ranged since the last refresh.
                //
                //   NMI  should climb by ~20 between refreshes (20 frames)
                //   RD   status-register reads; the NMI handler must do one
                //        per frame to clear the flag and release the line
                //   PC range collapsing to a few bytes means a tight wait loop
                static uint32_t prev_nmi = 0, prev_rd = 0;
                char l2[48];
                snprintf(l2, sizeof(l2),
                         "F%lu A%lu N%lu R%lu ST%02X",
                         (unsigned long)(vdp_frame_flags  % 100000),
                         (unsigned long)(vdp_irq_asserts  % 100000),
                         (unsigned long)(cv_nmi_count     % 100000),
                         (unsigned long)(vdp_status_reads % 100000),
                         vdp.status);
                (void)prev_nmi; (void)prev_rd;
                prev_nmi = cv_nmi_count;
                prev_rd  = vdp_status_reads;
                menu_debug_line(ROWS_DEBUG + 1, l2);

                // Third line: the bytes the CPU is sitting on, and the top of
                // the stack.
                //
                //   18 FE          JR $  -- a deliberate hang, so the game
                //                  reached its own error trap
                //   C3 xx xx       JP to itself, same thing
                //   FF FF FF FF    executing unmapped memory; it jumped into
                //                  nothing and the "loop" is RST 38 recursion
                //
                // The stack words are the most useful part: the top one is
                // usually the return address of whatever called into this,
                // which names the code path that went wrong.
                char l3[48];
                snprintf(l3, sizeof(l3),
                         "lastNMI@%04X rd%lu vec%04X",
                         cv_last_nmi_pc,
                         (unsigned long)(vdp_status_reads - cv_reads_at_nmi),
                         cv_nmi_vector);
                menu_debug_line(ROWS_DEBUG - 1, l3);

                pc_lo = 0xFFFF; pc_hi = 0x0000;
            }

            // Track how far the PC wanders between refreshes. A wide range is
            // normal execution; a range of a few bytes is a spin loop.
            if (z80.pc.w < pc_lo) pc_lo = z80.pc.w;
            if (z80.pc.w > pc_hi) pc_hi = z80.pc.w;
        }
#endif

        // Heartbeat: one LED toggle per second while the emulator loop runs.
        //
        // Without it a frozen picture is indistinguishable from a dead board,
        // and the two need completely different investigation. If the picture
        // stops while this keeps ticking, the RP2350 is fine and the emulated
        // machine has locked up -- a CPU or VDP emulation bug. If it stops
        // too, the host has hung or faulted.
        {
            static uint32_t beat = 0;
            if (++beat >= 60) {
                beat = 0;
                static bool on = false;
                on = !on;
                gpio_put(PIN_LED, on ? 0 : 1);
            }
        }

        // Pace to the display. The VDP frame is 59.92 Hz and DVI is 60 Hz, so
        // roughly one duplicated frame every 12 seconds -- imperceptible, and
        // far better than tearing.
        video_wait_vsync();
    }
}

// ---------------------------------------------------------------------------
int main(void) {
    // 252 MHz needs a small core voltage bump on RP2350.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);
    set_sys_clock_khz(CV_SYS_CLK_KHZ, true);
    setup_clocks();

    // Power the USB-A ports FIRST, and unconditionally -- not inside
    // usb_host_prepare(), and not behind ENABLE_USB_HOST.
    //
    // Two reasons. The CH334F hub needs time to come up before anything tries
    // to talk to it. And more importantly, an unpowered hub leaves D+/D-
    // floating: Pico-PIO-USB samples the bus line state during init, and a
    // floating bus with nothing holding it at a defined idle level can spin
    // forever inside tuh_init(). A power fault therefore presents as a host
    // stack hang, which sent me looking in entirely the wrong place.
    //
    // Doing it here also means the ports are powered even with the host
    // compiled out, so "do the jack LEDs light?" can be tested on its own.
    gpio_init(PIN_USB_HOST_5V_EN);
    gpio_set_dir(PIN_USB_HOST_5V_EN, GPIO_OUT);
    gpio_put(PIN_USB_HOST_5V_EN, USB_HOST_5V_ACTIVE_HIGH ? 1 : 0);
    sleep_ms(250);              // generous: the hub is slow to enumerate

    stdio_init_all();

    gpio_init(PIN_BUTTON1); gpio_set_dir(PIN_BUTTON1, GPIO_IN); gpio_pull_up(PIN_BUTTON1);
    gpio_init(PIN_BUTTON2); gpio_set_dir(PIN_BUTTON2, GPIO_IN); gpio_pull_up(PIN_BUTTON2);
    gpio_init(PIN_BUTTON3); gpio_set_dir(PIN_BUTTON3, GPIO_IN); gpio_pull_up(PIN_BUTTON3);

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

#if BOOT_DIAGNOSTICS
    // One long pulse, so a stale flash is obvious before anything else runs.
    gpio_put(PIN_LED, 0);
    sleep_ms(1000);
    gpio_put(PIN_LED, 1);
    sleep_ms(600);
#endif

    blink(1);                       // reached main, clocks up

    // USB first, video second. Pico-PIO-USB bit-bangs USB in PIO and its
    // initialisation is timing-critical; bringing HSTX up beforehand means the
    // video DMA interrupt is firing ~63k times a second throughout tuh_init(),
    // which is enough to hang it. Nothing in USB init needs the display, so
    // the ordering costs nothing.
    // Record the PIO-USB configuration BEFORE the device stack initialises
    // TinyUSB. tuh_configure() only stores settings; whichever path brings the
    // host up later then finds the correct pins already in place.
    usb_host_prepare();

    // Decide whether to offer the SD card as a USB drive before the device
    // stack comes up -- once tud_init() has run the port is already
    // enumerating, so this has to be settled first.
    const bool msc_wanted = wait_for_msc_request();
    usb_msc_set_enabled(msc_wanted);

    usb_msc_init();
    blink(2);                       // device stack up

#if !USB_HOST_ON_CORE1
    // Host on core 0, before video, so PIO-USB initialises with no video
    // interrupt running at all.
    usb_host_init();
#endif

#if ENABLE_VIDEO
    video_init();
#endif
    blink(3);                       // video up (or skipped)
    menu_message("ADAFRUIT COLECOJAM", "Starting up...", nullptr, false);

#if VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
    // video_init() has already taken core 1 for the HDMI output loop, so the
    // USB host comes up here on core 0.
    usb_host_init();
    host_ready = true;
#else
    // Core 1 brings up the USB host and audio. Wait for the host stack so a
    // hang there still shows as a missing blink rather than surfacing later.
    multicore_launch_core1(core1_main);
    for (int i = 0; i < 300 && !host_ready; i++) sleep_ms(10);
#endif
    blink(4);                       // USB host + audio running

    // --- SD card ---------------------------------------------------------
    if (!sd_init()) {
        int e = sd_last_error();
        fatal(e ? e : 1,
              e == 1 ? "No SD card responding."
            : e == 2 ? "SD card failed the voltage check."
            : e == 4 ? "SD card is stuck busy."
                     : "SD card did not finish initialising.",
              "Reseat the card, or try another one.");
    }

    if (f_mount(&fs, "", 1) != FR_OK)
        fatal(9, "Could not mount the SD card.", "Format it as FAT32 and retry.");

    // --- BIOS ------------------------------------------------------------
    menu_message("ADAFRUIT COLECOJAM", "Loading COLECO.BIN...", nullptr, false);
    uint32_t bios_len = load_file(COLECO_BIOS_FILE, bios_rom, sizeof(bios_rom));
    if (bios_len < CV_BIOS_SIZE)
        fatal(5, "coleco/COLECO.BIN is missing or short.",
              "An 8 KB ColecoVision BIOS is required.");

    if (!cv_init(bios_rom, bios_len))
        fatal(6, "Emulator init failed.", "Check COLECO.BIN.");

    blink(5);                       // SD mounted, BIOS loaded

    // --- Cartridge shield takes priority over the SD card ----------------
#if ENABLE_CART_READER
    menu_message("ADAFRUIT COLECOJAM", "Checking cartridge slot...", nullptr, false);
    if (cart_present()) {
        menu_message("CARTRIDGE DETECTED", "Reading cartridge ROM...", nullptr, false);
        cart_len = cart_read(cart_rom, sizeof(cart_rom));
        if (cart_len >= 0x2000) {
            cv_load_rom(cart_rom, cart_len);
            cv_reset();
            run_emulator();
        }
        // Fall through to the browser if the read came back short.
    }
#endif

    // --- ROM browser -----------------------------------------------------
    // Each stage updates the screen, so if it stops the last message on the
    // display names the exact step rather than leaving an earlier one frozen.
    menu_message("ADAFRUIT COLECOJAM", "Scanning /coleco for ROMs...", nullptr, false);
    int n = menu_scan_roms();
    if (n < 0)
        fatal(7, "No /coleco folder on the SD card.",
              "Create it and copy your .ROM files in.");

    menu_message("ADAFRUIT COLECOJAM", "Starting cartridge browser...",
                 "If this sticks, USB host is hanging.", false);

    char path[MAX_FILENAME_LEN + 16];
    if (!menu_select_rom(path, sizeof(path)))
        fatal(8, "No cartridge selected.", nullptr);

    // Let go of the launch press before the machine starts, so it is not
    // delivered to the game as a keypad digit.
    wait_for_button_release();

    menu_message("LOADING", path, nullptr, false);
    cart_len = load_file(path, cart_rom, sizeof(cart_rom));
    if (cart_len == 0)
        fatal(8, "Could not read that ROM file.", path);

    cv_load_rom(cart_rom, cart_len);
    cv_reset();
    run_emulator();
}
