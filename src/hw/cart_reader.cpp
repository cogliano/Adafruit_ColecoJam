// cart_reader.cpp -- ColecoVision Game Cartridge Adapter Shield
//
// Hardware and pin assignments follow the reference implementation:
//   https://github.com/cogliano/Fruit_Jam_ColecoVision_Cartridge_Reader
//
// A ColecoVision cartridge exposes 15 address lines, 8 data lines and four
// active-low chip selects, one per 8 KB bank at 0x8000, 0xA000, 0xC000 and
// 0xE000. The Fruit Jam header cannot spare 27 pins, so the shield drives the
// address bus through two chained 74HC595 shift registers.
//
// Those give 16 outputs for 15 address lines, and the spare bit carries CS
// 0xE000. Only three chip selects need real GPIO.
//
// Shift register wiring (74HC595 #1 feeds #2 through QH'):
//
//   chip #2   QH   QG   QF   QE   QD   QC   QB   QA
//             CS3  A14  A13  A12  A11  A10  A9   A8
//   chip #1   QH   QG   QF   QE   QD   QC   QB   QA
//             A7   A6   A5   A4   A3   A2   A1   A0
//
// The first bit clocked out travels furthest, so sending a 16-bit word MSB
// first with CS3 in bit 15 and A14..A0 in bits 14..0 puts every signal on the
// right output. Checked against the reference wiring table.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cart_reader.h"
#include "config.h"

#if ENABLE_CART_READER

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <string.h>

// Data bus D0..D7 and chip selects for 0x8000 / 0xA000 / 0xC000.
static const uint8_t data_pins[8] = CART_DATA_PINS;
static const uint8_t cs_pins[3]   = CART_CS_PINS;

// CS 0xE000 occupies bit 15 of the shift register word. Active low: SET to
// deselect, CLEARED to select.
#define SR_BIT_CS3      15
#define SR_ADDR_MASK    0x7FFF          // bits 0..14 carry A0-A14

static bool pins_ready = false;

// The first two bytes seen through EACH of the four chip selects during the
// last cart_present() probe: [cs0 b0, cs0 b1, cs1 b0, cs1 b1, ...].
//
// Probing all four rather than just bank 0 answers two questions at once. All
// zeroes everywhere means nothing is driving the data bus. A signature showing
// up under a chip select other than the first means the CS ordering is wrong,
// which the reference source cannot settle because it holds those pins in a
// Python set with no defined order.
static uint8_t probe_bytes[8];
static bool    probe_valid = false;

// Which chip select carried the cartridge header, or -1 if none did.
static int     header_bank = -1;

// ---------------------------------------------------------------------------
// Shift register
// ---------------------------------------------------------------------------
// Clocked out over SPI1 on the 2x16 header. This is a separate bus from the
// microSD card's SPI0, so no baud juggling or arbitration is needed.
static void shift_out16(uint16_t word) {
    const uint8_t tx[2] = {
        (uint8_t)(word >> 8),           // chip #2: CS3 and A14..A8
        (uint8_t)(word & 0xFF),         // chip #1: A7..A0
    };

    spi_write_blocking(CART_SPI_PORT, tx, 2);

    // Latch both chips' shift stages onto their outputs.
    gpio_put(PIN_CART_LATCH, 1);
    sleep_us(1);
    gpio_put(PIN_CART_LATCH, 0);
}

// Present an address with CS 0xE000 deselected -- the usual case.
static inline void set_address(uint16_t addr) {
    shift_out16((uint16_t)((addr & SR_ADDR_MASK) | (1u << SR_BIT_CS3)));
}

// ---------------------------------------------------------------------------
// Pin setup and teardown
// ---------------------------------------------------------------------------
static void cart_gpio_init(void) {
    if (pins_ready) return;

    gpio_init(PIN_CART_LATCH);
    gpio_set_dir(PIN_CART_LATCH, GPIO_OUT);
    gpio_put(PIN_CART_LATCH, 0);

    // SPI1 is ours alone -- nothing else on the board uses it once the shield
    // is fitted, so initialise it here rather than borrowing the SD card's.
    spi_init(CART_SPI_PORT, CART_SPI_BAUD);
    gpio_set_function(PIN_CART_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_CART_MOSI, GPIO_FUNC_SPI);

    // Data bus: inputs, never driven. The reference uses pull-downs, so an
    // unpopulated bank reads back as 0x00 rather than 0xFF.
    for (int i = 0; i < 8; i++) {
        gpio_init(data_pins[i]);
        gpio_set_dir(data_pins[i], GPIO_IN);
        gpio_pull_down(data_pins[i]);
    }

    for (int i = 0; i < 3; i++) {
        gpio_init(cs_pins[i]);
        gpio_set_dir(cs_pins[i], GPIO_OUT);
        gpio_put(cs_pins[i], 1);        // deselected
    }

    // Park the shift register with CS 0xE000 deselected.
    shift_out16((uint16_t)(1u << SR_BIT_CS3));

    pins_ready = true;
}

// The shield shares pins with the audio codec's I2C bus (GPIO 20/21) and the
// debug UART (GPIO 8/9). Hand them back when no cartridge is found, otherwise
// a probe that finds nothing leaves both dead for the rest of the run. The
// codec is configured long before this runs, so its settings survive.
static void cart_gpio_release(void) {
    if (!pins_ready) return;

    for (int i = 0; i < 8; i++) gpio_disable_pulls(data_pins[i]);

    // Hand SPI1's pins back to the ESP32-C6 link they normally serve.
    spi_deinit(CART_SPI_PORT);
    gpio_set_function(PIN_CART_SCK,  GPIO_FUNC_NULL);
    gpio_set_function(PIN_CART_MOSI, GPIO_FUNC_NULL);

    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_SDA);
    gpio_pull_up(PIN_I2C_SCL);

    pins_ready = false;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
static uint8_t read_data_bus(void) {
    // gpio_get_all64(), NOT gpio_get_all().
    //
    // The RP2350B has 48 GPIO across two banks, and gpio_get_all() returns a
    // 32-bit mask covering only 0-31. Four of this shield's data lines sit on
    // GPIO 41, 42, 44 and 45, so a 32-bit read silently truncated them to zero
    // -- half the data bus permanently reading 0 while the rest worked.
    const uint64_t all = gpio_get_all64();
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        if (all & (1ull << data_pins[i])) v |= (uint8_t)(1 << i);
    return v;
}

// Read one byte from bank 0-3 at `off` within that 8 KB bank.
//
// The address presented is (bank << 13) | off, matching the reference: A13 and
// A14 are driven as well as the chip select, which matters for cartridges
// built from one large ROM rather than several 8 KB parts.
static uint8_t cart_read_once(int bank, uint16_t off) {
    const uint16_t addr = (uint16_t)(((bank & 3) << 13) | (off & 0x1FFF));
    uint8_t v;

    if (bank < 3) {
        set_address(addr);
        gpio_put(cs_pins[bank], 0);
        sleep_us(CART_ACCESS_US);       // 595 propagation + ROM access time
        v = read_data_bus();
        gpio_put(cs_pins[bank], 1);
    } else {
        // CS 0xE000 shares the shift register with the address, so one latch
        // would make both valid at the same instant. Two latches keep the
        // address settled before the ROM is selected.
        shift_out16((uint16_t)(addr | (1u << SR_BIT_CS3)));
        sleep_us(1);
        shift_out16(addr);              // CS3 low
        sleep_us(CART_ACCESS_US);
        v = read_data_bus();
        shift_out16((uint16_t)(addr | (1u << SR_BIT_CS3)));
    }
    return v;
}

// Read one byte from bank 0-3 at `off` within that 8 KB bank.
//
// With CART_VERIFY_READS the byte is read twice and the two must agree. A
// marginal access does not produce random garbage -- it produces a value that
// decays towards zero as bits fail to settle -- so two reads that match is a
// good signal that the bus had time to stabilise. Retrying with the same
// timing works because the ROM is already selected and warm by the second
// attempt.
static uint8_t cart_read_byte(int bank, uint16_t off) {
#if CART_VERIFY_READS
    uint8_t a = cart_read_once(bank, off);
    for (int attempt = 0; attempt < 3; attempt++) {
        const uint8_t b = cart_read_once(bank, off);
        if (a == b) return a;
        a = b;
    }
    return a;
#else
    return cart_read_once(bank, off);
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool cart_present(void) {
    cart_gpio_init();

    // This shield has no cartridge-detect line, so presence is decided by the
    // header signature. Every ColecoVision cartridge starts with AA 55 (show
    // the BIOS title screen) or 55 AA (skip it).
    // Probe layout, chosen so one line of hex answers the two questions that
    // matter:
    //
    //   [0..3]  bank 0 at 0x0000, 0x0001, 0x1555, 0x1AAA
    //           Does the ADDRESS do anything? Those last two flip every
    //           address line in opposite patterns. If all four bytes are
    //           identical the shift registers are not latching and nothing
    //           downstream is worth interpreting.
    //   [4..6]  banks 1, 2, 3 at offset 0 -- does the CHIP SELECT do anything?
    //   [7]     bank 0 at 0x0000 again, to show the reading is stable
    //
    static const uint16_t probe_addr[4] = { 0x0000, 0x0001, 0x1555, 0x1AAA };
    for (int i = 0; i < 4; i++)
        probe_bytes[i] = cart_read_byte(0, probe_addr[i]);
    for (int bank = 1; bank < 4; bank++)
        probe_bytes[3 + bank] = cart_read_byte(bank, 0);
    probe_bytes[7] = cart_read_byte(0, 0x0000);
    probe_valid = true;

    header_bank = -1;
    for (int bank = 0; bank < 4; bank++) {
        const uint8_t b0 = cart_read_byte(bank, 0);
        const uint8_t b1 = cart_read_byte(bank, 1);
        if ((b0 == 0xAA && b1 == 0x55) || (b0 == 0x55 && b1 == 0xAA)) {
            header_bank = bank;
            break;
        }
    }

    if (header_bank < 0) cart_gpio_release();
    return header_bank >= 0;
}

int cart_header_bank(void) { return header_bank; }

int cart_probe_bytes(uint8_t *dst, int max) {
    if (!probe_valid) return 0;
    int n = (max < 8) ? max : 8;
    for (int i = 0; i < n; i++) dst[i] = probe_bytes[i];
    return n;
}

uint32_t cart_read(uint8_t *dst, uint32_t max_len) {
    cart_gpio_init();

    // Start from whichever chip select actually held the header. Normally 0,
    // but if the CS ordering differs from the assumed one this still dumps the
    // banks in the right sequence.
    const int first = (header_bank > 0) ? header_bank : 0;

    uint32_t total = 0;
    for (int i = 0; i < 4; i++) {
        const int bank = (first + i) & 3;
        // Decide whether this bank is populated by looking for VARIATION, not
        // for a particular value.
        //
        // An unpopulated bank leaves the data bus floating, and the obvious
        // test -- "does it read all 0x00?" -- does not hold on this hardware.
        // RP2350 A2 erratum E9 means a floating input with only the internal
        // pull-down can latch high, and an empty slot in fact reads a constant
        // 0xC0 here. Testing for zero would call every bank populated and
        // report every cartridge as a full 32 KB.
        //
        // Whatever value a floating bus settles on, it is the SAME value at
        // every address, because nothing is driving it. Real ROM contents
        // vary. So: sample across the bank and treat it as empty if every
        // sample is identical.
        //
        // 32 samples spread through the 8 KB, at prime-ish strides so a
        // pathologically repetitive ROM is unlikely to fool it.
        const uint8_t first_sample = cart_read_byte(bank, 0x0000);
        bool uniform = true;
        for (int j = 1; j < 32 && uniform; j++) {
            const uint16_t off = (uint16_t)((j * 0x0107) & 0x1FFF);
            if (cart_read_byte(bank, off) != first_sample) uniform = false;
        }
        if (uniform && i > 0) break;

        for (uint32_t off = 0; off < 0x2000; off++) {
            if (total >= max_len) { cart_gpio_release(); return total; }
            dst[total++] = cart_read_byte(bank, (uint16_t)off);
        }
    }

    // Done with the cartridge. Release the pins so the audio codec's I2C bus,
    // the debug UART and SPI1 go back to their normal owners for the rest of
    // the session. The ROM now lives entirely in RAM and the shield is never
    // touched again -- which also means a cartridge can be pulled while a game
    // is running without upsetting anything.
    cart_gpio_release();
    return total;
}

#else  // !ENABLE_CART_READER

bool     cart_present(void) { return false; }
uint32_t cart_read(uint8_t *dst, uint32_t max_len) { (void)dst; (void)max_len; return 0; }
int      cart_probe_bytes(uint8_t *dst, int max)   { (void)dst; (void)max; return 0; }
int      cart_header_bank(void) { return -1; }

#endif
