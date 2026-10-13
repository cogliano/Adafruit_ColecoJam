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
#include "emu/eeprom24.h"
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
#include "hw/sfx.h"
#include "hw/psram.h"
#include "hw/bootloader.h"
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
// Cartridges, of every size, are loaded into PSRAM rather than SRAM.
//
// A 32 KB buffer here used to hold ordinary cartridges while only MegaCarts
// went to PSRAM. Keeping both was 32 KB of SRAM spent on a copy of something
// PSRAM already holds comfortably, and SRAM is the scarce resource on this
// board -- that 32 KB is what lets the title chime exist.
//
// MegaCart ROMs have been executing from PSRAM for a while now, so there is no
// open question about whether the emulator can read cartridge bytes from it at
// speed. The cost is that PSRAM becomes required to load anything at all,
// which is reported plainly at boot if it ever fails.
#define cart_buffer()       cj_psram_base()
#define cart_buffer_size()  cj_psram_bytes()
static uint32_t cart_len = 0;

// Set when a cartridge game has already run, so the boot title sequence is not
// played on the way into the menu afterwards.
static bool cart_played = false;

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
            cv_render_audio(mix_buf, AUDIO_BUF_SAMPLES);
            sfx_mix(mix_buf, AUDIO_BUF_SAMPLES);
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
            cv_render_audio(mix_buf, AUDIO_BUF_SAMPLES);
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

    // Wait for release, so a press that runs long cannot carry over into the
    // menu -- where holding Button 1 now means "leave ColecoJam".
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
// Size of a file, or 0 if it cannot be opened. Used to decide where a
// cartridge should be loaded before any of it is read.
static uint32_t file_size(const char *path) {
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return 0;
    return (uint32_t)fno.fsize;
}

#if ENABLE_ACTIVISION
// --- EEPROM save files -----------------------------------------------------
//
// Beside the ROM, same name with .sav. Only for games loaded from the SD card:
// a cartridge in the shield has its own real EEPROM on board, and writing a
// save file for it would be recording the wrong chip's contents.
static char sav_path[256];

static void sav_path_for(const char *rom_path) {
    sav_path[0] = '\0';
    if (!rom_path) return;

    const size_t n = strlen(rom_path);
    if (n + 5 >= sizeof(sav_path)) return;      // no room for the extension

    strcpy(sav_path, rom_path);
    char *dot = strrchr(sav_path, '.');
    const char *slash = strrchr(sav_path, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';   // only a real extension
    strcat(sav_path, ".sav");
}

// Load a save file, sizing the chip from the file when one exists. That makes
// the file itself the record of which EEPROM a game has, so EEPROM_DEFAULT_BYTES
// only matters the first time.
static void eeprom_load(const char *rom_path) {
    sav_path_for(rom_path);
    eeprom_attach(EEPROM_DEFAULT_BYTES, cj_psram_scratch());
    if (!sav_path[0] || !eeprom_size()) return;   // detached: nothing to load

    FILINFO fno;
    if (f_stat(sav_path, &fno) != FR_OK) return;

    const uint32_t size = (fno.fsize == EEPROM_24C08_BYTES)
                        ? EEPROM_24C08_BYTES : EEPROM_24C256_BYTES;
    eeprom_attach(size, cj_psram_scratch());

    FIL f;
    if (f_open(&f, sav_path, FA_READ) != FR_OK) return;
    UINT br = 0;
    f_read(&f, eeprom_data(), size, &br);
    f_close(&f);
    eeprom_clear_dirty();
    printf("EEPROM: loaded %u bytes from %s\n", (unsigned)br, sav_path);
}

// Write the save file, but only if the game actually changed something.
static void eeprom_save(void) {
    if (!sav_path[0] || !eeprom_size() || !eeprom_dirty()) return;

    FIL f;
    if (f_open(&f, sav_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("EEPROM: could not write %s\n", sav_path);
        return;
    }
    UINT bw = 0;
    f_write(&f, eeprom_data(), eeprom_size(), &bw);
    f_close(&f);
    eeprom_clear_dirty();
    printf("EEPROM: wrote %u bytes to %s\n", (unsigned)bw, sav_path);
}
#endif  // ENABLE_ACTIVISION

#if ENABLE_MEGACART
// A megabyte through the shield takes a while; show how far it has got.
static void cart_dump_progress(uint32_t done, uint32_t total) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Reading cartridge... %lu of %lu KB",
             (unsigned long)(done / 1024), (unsigned long)(total / 1024));
    menu_message("CARTRIDGE", msg, nullptr, false);
}
#endif

// Copy the cartridge in the shield to /coleco, as game.rom, game2.rom, ...
//
// Reads it the same way loading does, so a bank-switched cartridge is copied
// whole rather than only its first 32 KB. Returns the name used, or nullptr.
static const char *copy_cartridge_to_sd(void) {
    static char path[64];

    if (!cj_psram_ready()) return nullptr;

    uint32_t len = 0;
#if ENABLE_MEGACART
    menu_message("COPY CARTRIDGE", "Checking for bank switching...", nullptr, false);
    len = cart_read_banked(cj_psram_base(), cj_psram_bytes(), cart_dump_progress);
#endif
    if (len == 0) {
        menu_message("COPY CARTRIDGE", "Reading cartridge...", nullptr, false);
        len = cart_read(cj_psram_base(), cj_psram_bytes());
    }
    if (len == 0) return nullptr;

    // First free name. f_stat rather than trying to open for writing, so an
    // existing file is never truncated by the test itself.
    FILINFO fno;
    for (int n = 1; n <= 999; n++) {
        if (n == 1) snprintf(path, sizeof(path), "/coleco/game.rom");
        else        snprintf(path, sizeof(path), "/coleco/game%d.rom", n);
        if (f_stat(path, &fno) != FR_OK) break;
        if (n == 999) return nullptr;              // give up rather than loop
    }

    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return nullptr;

    uint32_t done = 0;
    while (done < len) {
        const UINT want = (len - done > 16384u) ? 16384u : (UINT)(len - done);
        UINT bw = 0;
        if (f_write(&f, cj_psram_base() + done, want, &bw) != FR_OK || bw == 0) {
            f_close(&f);
            return nullptr;
        }
        done += bw;

        char msg[48];
        snprintf(msg, sizeof(msg), "Writing... %lu of %lu KB",
                 (unsigned long)(done / 1024), (unsigned long)(len / 1024));
        menu_message("COPY CARTRIDGE", msg, path, false);
    }
    f_close(&f);
    return (done == len) ? path : nullptr;
}

// Load a cartridge into PSRAM.
//
// Read in chunks rather than one call: a 1 MB read would tie up the SD driver
// for a long stretch, and this way the screen can show progress instead of
// looking hung.
static uint32_t load_file_psram(const char *path, uint32_t len) {
    if (!cj_psram_ready() || len > cj_psram_bytes()) return 0;

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;

    uint8_t *dst = cj_psram_base();
    uint32_t done = 0;
    while (done < len) {
        const UINT want = (len - done > 16384u) ? 16384u : (UINT)(len - done);
        UINT br = 0;
        if (f_read(&f, dst + done, want, &br) != FR_OK || br == 0) break;
        done += br;

        if ((done & 0x3FFFFu) == 0) {           // every 256 KB
            char msg[48];
            snprintf(msg, sizeof(msg), "Loading cartridge... %lu KB",
                     (unsigned long)(done / 1024));
            menu_message("LOADING", msg, nullptr, false);
        }
    }
    f_close(&f);
    return done;
}

static uint32_t load_file(const char *path, uint8_t *dst, uint32_t max_len) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;

    UINT br = 0;
    FRESULT r = f_read(&f, dst, max_len, &br);
    f_close(&f);
    return (r == FR_OK) ? (uint32_t)br : 0;
}

// Leave the emulator. Under the pico-bootLoader the useful destination is its
// picker, not the RP2350 USB drive -- the user picked us from a menu and expects
// to get back to it. Standalone there is no menu to return to, so fall through
// to BOOTSEL as before. The test is made at runtime, so one binary is right
// both ways.
[[noreturn]] static void coleco_exit(void) {
    if (coleco_launched_from_bootloader()) coleco_return_to_bootloader();
    reset_usb_boot(0, 0);
    for (;;) tight_loop_contents();   // reset_usb_boot() does not return
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
                if (held >= 20) coleco_exit();
            }
            sleep_ms(10);
        }
    }
}

// ---------------------------------------------------------------------------
// Emulator loop
// ---------------------------------------------------------------------------
// Runs the loaded cartridge until Button 1 is pressed, then returns so the
// caller can show the cartridge menu again.
static void run_emulator(void) {
    uint16_t *fb = video_framebuffer();

#if AUDIO_SINK == AUDIO_SINK_HDMI
    // Samples carried over between frames. Cleared on every entry, or the tail
    // of the previous game's sound plays at the start of the next one.
    static int16_t pend_buf[AUDIO_BUF_SAMPLES];
    int            pend_len = 0;
#endif

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
            for (int guard = 0; guard < 8; guard++) {
                if (hdmi_audio_queue_level() >= HDMI_AUDIO_QUEUE_TARGET) break;

                if (pend_len == 0) {
                    cv_render_audio(pend_buf, AUDIO_BUF_SAMPLES);
                    // Any effect still sounding rides over the game audio.
                    sfx_mix(pend_buf, AUDIO_BUF_SAMPLES);
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

        // Button 1: leave the game and return to the cartridge menu, at once.
        //
        // Leaving ColecoJam entirely -- coleco_exit(), back to the
        // pico-bootLoader picker or the UF2 bootloader -- is now a one-second
        // HOLD of Button 1 in the menu instead. Two presses from a game: tap
        // to reach the menu, hold to leave.
        if (!gpio_get(PIN_BUTTON1)) break;

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
                         "PC%04X SP%04X bk%02lu HL%04X A%02X %s",
                         z80.pc.w, z80.sp.w,
                         (unsigned long)cv_rom_cur_bank(),
                         z80.hl.w, z80.af.b.h,
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
                         // R1 bit 6 is the display enable: a black screen
                         // with the machine still running is usually the game
                         // having switched the display off and not come back.
                         // The bytes at PC say whether it is executing real
                         // code or has wandered into empty space.
                         "R1=%02X M%02X %02X %02X @%04X",
                         vdp.reg[1],
                         cv_bus_read(z80.pc.w),
                         cv_bus_read((uint16_t)(z80.pc.w + 1)),
                         cv_bus_read((uint16_t)(z80.pc.w + 2)),
                         cv_last_nmi_pc);
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

    // --- Leaving the game ------------------------------------------------
    // Silence the sound chip. With the codec sink, core 1 keeps rendering the
    // PSG regardless of what core 0 is doing, so the last notes of the game
    // would otherwise drone on under the menu indefinitely.
    psg_reset();

    // Wait for the button to come up, so one press is exactly one action --
    // and so it cannot start counting towards the menu's hold-to-exit. Same
    // 2-second guard as the launch press, so a stuck button cannot hang this.
    absolute_time_t deadline = make_timeout_time_ms(2000);
    while (!gpio_get(PIN_BUTTON1) && !time_reached(deadline)) sleep_ms(5);
    sleep_ms(30);                   // debounce

    // The LED heartbeat may have been left lit mid-toggle.
    gpio_put(PIN_LED, 1);
    printf("Returned to menu\n");
}

// ---------------------------------------------------------------------------
int main(void) {
    // 252 MHz needs a small core voltage bump on RP2350.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);
    set_sys_clock_khz(CV_SYS_CLK_KHZ, true);
    setup_clocks();

    // Immediately after the clock change and before core 1 exists.
    //
    // Both matter. The PSRAM timing is derived from clk_sys, so it has to be
    // redone once clk_sys is final; and psram_reinitialize() reaches into the
    // QMI, which is not something to do while the other core is running the
    // video output. Nothing can be shown on screen this early, so the result
    // is reported further down.
    cj_psram_setup();

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

    // Console banner, printed before anything that can fail. If this never
    // appears, the problem is the wiring or the UART setup, not something
    // later in boot.
    printf("\nAdafruit ColecoJam %s (%s %s)\n", ACJ_BUILD_ID, __DATE__, __TIME__);

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
    // Holding Button 1 at boot means either "offer the SD card as a USB drive"
    // or, with a cartridge fitted, "show me the boot menu". Which of the two
    // is not known yet -- the cartridge cannot be probed this early and there
    // is no display to ask on -- so the device stack is brought up ready for
    // MSC either way, and switched off further down if the menu says
    // otherwise. The alternative would be deciding after tud_init(), by which
    // time the port is already enumerating as something.
    const bool button_at_boot = wait_for_msc_request();
    usb_msc_set_enabled(button_at_boot);

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

    // --- boot menu -------------------------------------------------------
    //
    // Only with a cartridge fitted: without one there is nothing to copy or
    // load, and Button 1 keeps its old meaning of asking for USB drive mode.
    bool want_msc  = button_at_boot;
    bool skip_cart = false;

    if (button_at_boot && cart_present()) {
        for (;;) {
            const int choice = menu_boot_options();

            if (choice == BOOT_OPT_USB) {
                want_msc  = true;
                skip_cart = true;               // the PC owns the card
                break;
            }
            if (choice == BOOT_OPT_LOAD) {
                want_msc = false;
                break;
            }

            // Copy the cartridge. Needs the card, so mount it here -- the
            // usual mount further down has not happened yet.
            want_msc = false;
            usb_msc_set_enabled(false);

            if (!sd_init() || f_mount(&fs, "", 1) != FR_OK) {
                menu_wait_any_input("COPY FAILED", "Could not read the SD card.",
                                    "Press any button.");
                continue;
            }
            f_mkdir("/coleco");                 // harmless if it exists

            const char *written = copy_cartridge_to_sd();
            if (written) {
                // Exact byte counts, not KB: a size that looks wrong needs to
                // be checked against the real file, and rounding hides what
                // the read and the trim each did.
                // Bytes and KB: the exact figure is what gets compared
                // against a known-good dump, the KB is what is easy to read.
                char detail[48];
                const unsigned long n = (unsigned long)cart_status.trimmed;
                snprintf(detail, sizeof(detail), "%lu bytes (%lu KB)",
                         n, n / 1024);
                menu_wait_any_input("COPY COMPLETE", written, detail);
            }
            else
                menu_wait_any_input("COPY FAILED",
                                    "Could not copy the cartridge.",
                                    "Press any button.");
        }
    }
    usb_msc_set_enabled(want_msc);

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

#if ENABLE_MEGACART
    // Before any cartridge is chosen, so an oversized one can be refused with
    // a clear message instead of failing halfway through loading.
    if (!cj_psram_ready()) {
        // Not fatal: standard cartridges are unaffected. Reported here rather
        // than where it happens, because that is before the display exists.
        // Fatal now: every cartridge is loaded into PSRAM, so without it
        // there is nothing this can go on to do.
        fatal(9, "PSRAM did not start.",
              "No cartridge can be loaded without it.");
    }
#endif

    blink(5);                       // SD mounted, BIOS loaded

    // --- Cartridge shield takes priority over the SD card ----------------
#if ENABLE_CART_READER
    menu_message("ADAFRUIT COLECOJAM", "Checking cartridge slot...", nullptr, false);
    if (!skip_cart && cart_present()) {
        menu_message("CARTRIDGE DETECTED", "Reading cartridge ROM...", nullptr, false);
        const uint8_t *cart_base = cart_buffer();

#if ENABLE_MEGACART
        // Try a bank-switched read first. It returns 0 for an ordinary
        // cartridge, which then loads into SRAM exactly as before.
        if (cj_psram_ready()) {
            menu_message("CARTRIDGE DETECTED", "Checking for bank switching...",
                         nullptr, false);
            cart_len = cart_read_banked(cj_psram_base(), cj_psram_bytes(),
                                        cart_dump_progress);
            if (cart_len > 0) {
                cart_base = cj_psram_base();
                printf("Cartridge: %lu KB banked, read into PSRAM\n",
                       (unsigned long)(cart_len / 1024));
            }
        }
        if (cart_len == 0)
#endif
        cart_len = cart_read(cart_buffer(), cart_buffer_size());
        printf("Cartridge read: %lu bytes\n", (unsigned long)cart_len);
        if (cart_len >= 0x2000) {
    #if ENABLE_ACTIVISION
            // Emulated so the game runs, but with no save file: the real
            // cartridge has its own EEPROM and this is not its contents.
            sav_path[0] = '\0';
            eeprom_attach(EEPROM_DEFAULT_BYTES, cj_psram_scratch());
#endif
            cv_load_rom(cart_base, cart_len);
            cv_reset();
            cart_played = true;
            run_emulator();
            // Button 1 pressed during the cartridge game: carry on into the
            // browser below, so SD games are reachable without pulling the
            // cartridge and resetting.
        }
        // Fall through to the browser if the read came back short.
    }
#endif

    // --- ROM browser -----------------------------------------------------
    // Browse, load, play -- and back round again whenever Button 1 is pressed
    // in a game. The SD card stays mounted and the BIOS stays loaded, so the
    // return is instant rather than a reboot. Each stage updates the screen,
    // so if one stops the message on the display names the exact step.
#if TITLE_SEQUENCE
    // Title sequence, once, before the menu is first shown.
    //
    // Skipped when a cartridge was fitted: that boots straight into the game,
    // and arriving here means Button 1 was pressed to leave it, where titles
    // would be an odd interruption rather than a boot animation.
    if (!cart_played) menu_title_sequence();
#endif

    bool first_pass = true;

    for (;;) {
        // Rescanned every time: files may have been added over USB since.
        //
        // The progress messages are skipped on the first pass. Scanning takes
        // a few milliseconds, so they appear only as a flash -- and after the
        // title sequence that flash lands between the titles fading out and
        // the menu fading in, which spoils the transition. On later passes,
        // returning from a game, they are worth having.
        if (!first_pass)
            menu_message("ADAFRUIT COLECOJAM", "Scanning /coleco for ROMs...",
                         nullptr, false);
        int n = menu_scan_roms();
        if (n < 0)
            fatal(7, "No /coleco folder on the SD card.",
                  "Create it and copy your .ROM files in.");

        // The first line after the cartridge probe, which borrows the UART
        // pins. A banner but no line here means the probe did not hand them
        // back.
        printf("ROM browser: %d ROM%s\n", n, n == 1 ? "" : "s");

        if (!first_pass)
            menu_message("ADAFRUIT COLECOJAM", "Starting cartridge browser...",
                         "If this sticks, USB host is hanging.", false);
        first_pass = false;

        char path[MAX_FILENAME_LEN + 16];
        if (!menu_select_rom(path, sizeof(path))) {
            // Button 1 held in the menu: leave ColecoJam -- to the
            // pico-bootLoader picker, or the UF2 bootloader when standalone.
            printf("Exit requested from menu\n");
            coleco_exit();
        }

        // Let go of the launch press before the machine starts, so it is not
        // delivered to the game as a keypad digit.
        wait_for_button_release();

        menu_message("LOADING", path, nullptr, false);

        // Anything over 32 KB is a MegaCart and goes to PSRAM; the rest loads
        // into the SRAM buffer exactly as before.
        const uint32_t fsize = file_size(path);
        const uint8_t *rom_base = cart_buffer();
        (void)fsize;                 // only consulted when MegaCart is built

        if (fsize > MEGACART_MAX_BYTES)
            fatal(8, "Cartridge is larger than 1 MB.", path);
        if (!cj_psram_ready())
            fatal(8, "PSRAM did not start; no cartridge can be loaded.", path);

        cart_len = load_file_psram(path, fsize);
        if (cart_len != fsize)
            fatal(8, "Could not read that ROM file.", path);
        printf("Cartridge: %lu KB in PSRAM\n", (unsigned long)(cart_len / 1024));
        printf("Starting %s (%lu bytes)\n", path, (unsigned long)cart_len);

        // cv_reset() clears RAM, the VDP, the PSG and the CPU, so the new game
        // starts exactly as it would from power-on -- nothing carries across
        // from the previous one.
#if ENABLE_ACTIVISION
        eeprom_load(path);
#endif
        cv_load_rom(rom_base, cart_len);

#if SHOW_CART_INFO
        // What the loader actually concluded, on screen. A ROM that is not a
        // whole power-of-two number of 16 KB banks is not a MegaCart and gets
        // truncated to 32 KB, which looks from the outside exactly like a
        // MegaCart that failed to work.
        {
            const uint32_t banks = cv_rom_banks();
            char l1[48], l2[48];
            snprintf(l1, sizeof(l1), "%lu KB in PSRAM",
                     (unsigned long)(cart_len / 1024));
            // The first bytes of bank 0 AND of the last bank. A MegaCart
            // carries its AA 55 header at the start of one or the other, and
            // showing both says whether the ROM lacks a header or whether what
            // we loaded simply is not what is in the file.
            if (banks) {
                const uint8_t *last = rom_base + (banks - 1) * 0x4000u;
                snprintf(l2, sizeof(l2), "b0 %02X %02X %02X  b%lu %02X %02X %02X",
                         rom_base[0], rom_base[1], rom_base[2],
                         (unsigned long)(banks - 1),
                         last[0], last[1], last[2]);
            } else {
                snprintf(l2, sizeof(l2), "not banked, hdr %02X %02X %02X",
                         rom_base[0], rom_base[1], rom_base[2]);
            }
            menu_message("CARTRIDGE", l1, l2, false);
            sleep_ms(CART_INFO_MS);
        }
#endif

        cv_reset();
        run_emulator();

#if ENABLE_ACTIVISION
        // On the way back to the browser, which is the only way out of a
        // game, so there is no path that loses a score.
        eeprom_save();
#endif
    }
}
