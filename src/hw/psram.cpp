// psram.cpp -- the board's 8 MB PSRAM, used to hold MegaCart ROMs
//
// A MegaCart runs to 1 MB and SRAM has nothing like that spare, but the Fruit
// Jam carries 8 MB of PSRAM. Reads are memory-mapped, so the emulator takes
// cartridge bytes straight from a pointer with no paging of our own.
//
// Only core 0 touches it. Core 1's video code lives in SRAM -- that is what
// building copy_to_ram buys -- and its scanout DMA reads the framebuffer from
// SRAM, so nothing here can disturb the HDMI timing.
//
// Nothing initialises the chip here. The SDK brings PSRAM up during runtime
// init from PICO_PSRAM_SIZE_BYTES and PICO_PSRAM_CS_PIN in the board header,
// before main() runs. This file only places the buffer and checks it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "psram.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/psram.h"
#include <stdio.h>

// PSRAM is used through its memory window directly rather than by placing a
// variable in it.
//
// The SDK's __uninitialized_psram macro would need the linker's PSRAM region
// and a reserved megabyte; using the window needs neither, and the ROM is
// written before anything reads it so there is nothing to initialise.
//
// The address is architectural -- QMI chip select 1 on the RP2350 -- but it is
// not assumed: psram_check_address() is asked about both ends of the range
// below, and if the SDK disagrees the feature switches itself off rather than
// reading from somewhere arbitrary.
#define PSRAM_WINDOW ((uint8_t *)0x11000000u)

namespace { bool ready = false; }

bool cj_psram_setup(void) {
    if (ready) return true;

    if (!psram_is_available()) {
        printf("PSRAM: not available\n");
        return false;
    }

    // Re-time the chip for the clock we are ACTUALLY running at.
    //
    // The SDK brings PSRAM up during runtime init, before main(), and picks
    // the QMI divider from the system clock in force at that moment. main()
    // then raises clk_sys to 252 MHz for the HDMI output, which doubles the
    // PSRAM clock and pushes it past the chip's 133 MHz limit.
    //
    // The result is not an obvious failure. Most reads are fine and the odd
    // one comes back with a bit flipped, so a ROM loads looking almost right
    // and the machine refuses to start it -- which is a miserable thing to
    // diagnose from the far end.
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != PICO_OK) {
        printf("PSRAM: could not work out timing for this clock\n");
        return false;
    }
    if (psram_reinitialize() != PICO_OK) {
        printf("PSRAM: re-initialise failed\n");
        return false;
    }
    // Both ends must be backed by real memory, which also confirms the window
    // address is where we think it is.
    if (!psram_check_address(PSRAM_WINDOW) ||
        !psram_check_address(PSRAM_WINDOW + MEGACART_MAX_BYTES - 1)) {
        printf("PSRAM: %u KB present, too small for a %u KB cartridge buffer\n",
               (unsigned)(psram_get_size() / 1024),
               (unsigned)(MEGACART_MAX_BYTES / 1024));
        return false;
    }

    // Prove it before trusting it, and prove it properly.
    //
    // The previous version wrote eight bytes at each end, which marginal
    // timing passes by luck -- a bit that flips one read in a thousand will
    // not show up in sixteen bytes. This writes 32 KB at each end from a
    // reproducible sequence and reads it back, which is about 65000 chances
    // to catch it and still only a few milliseconds.
    uint8_t *const mem = PSRAM_WINDOW;
    const size_t   span = 32u * 1024u;
    const size_t   spots[2] = { 0, MEGACART_MAX_BYTES - span };

    for (int s2 = 0; s2 < 2; s2++) {
        uint32_t lcg = 0x12345678u + (uint32_t)s2;
        for (size_t i = 0; i < span; i++) {
            lcg = lcg * 1103515245u + 12345u;
            mem[spots[s2] + i] = (uint8_t)(lcg >> 16);
        }
    }
    for (int s2 = 0; s2 < 2; s2++) {
        uint32_t lcg = 0x12345678u + (uint32_t)s2;
        for (size_t i = 0; i < span; i++) {
            lcg = lcg * 1103515245u + 12345u;
            if (mem[spots[s2] + i] != (uint8_t)(lcg >> 16)) {
                printf("PSRAM: read-back failed at %u (got %02X want %02X)\n",
                       (unsigned)(spots[s2] + i), mem[spots[s2] + i],
                       (uint8_t)(lcg >> 16));
                return false;
            }
        }
    }

    ready = true;
    printf("PSRAM: %u KB available, %u KB cartridge buffer ready\n",
           (unsigned)(psram_get_size() / 1024),
           (unsigned)(MEGACART_MAX_BYTES / 1024));
    return true;
}

uint8_t *cj_psram_base(void)  { return ready ? PSRAM_WINDOW : nullptr; }
size_t   cj_psram_bytes(void) { return ready ? MEGACART_MAX_BYTES : 0; }
bool     cj_psram_ready(void) { return ready; }
