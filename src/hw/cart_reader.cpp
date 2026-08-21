// cart_reader.cpp -- Adafruit ColecoJam cartridge reader shield
//
// Reference design (CircuitPython):
//   https://github.com/cogliano/Fruit_Jam_ColecoVision_Cartridge_Reader
//
// A ColecoVision cartridge edge connector carries 15 address lines, 8 data
// lines, four active-low chip selects (one per 8 KB bank at 0x8000, 0xA000,
// 0xC000, 0xE000), Vcc and two grounds. The Fruit Jam's 2x16 header exposes
// far fewer than 27 usable GPIO, so -- exactly as in the well-known Arduino
// reader this shield descends from -- the 15 address lines are driven through
// a pair of daisy-chained 74HC595 shift registers, leaving the 8 data lines
// and 4 chip selects on direct GPIO.
//
// >>> PIN ASSIGNMENTS BELOW ARE THE DEFAULTS AND MUST BE CONFIRMED <<<
// I was not able to read the shield's pin table directly, so these follow the
// Fruit Jam's broken-out header in the obvious order. Cross-check them against
// the CircuitPython source before connecting a cartridge -- driving a data pin
// as an output into the cartridge's output could damage either board. Every
// pin is a single #define, so correcting them is a one-line change each.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cart_reader.h"
#include "config.h"

#if ENABLE_CART_READER

#include "pico/stdlib.h"
#include <string.h>

// --- Shift register (address bus A0-A14) -----------------------------------
#define PIN_SR_DATA    6      // 74HC595 SER
#define PIN_SR_CLOCK   7      // 74HC595 SRCLK
#define PIN_SR_LATCH   10     // 74HC595 RCLK

// --- Data bus D0-D7 (inputs) ----------------------------------------------
static const uint8_t data_pins[8] = { 20, 21, 28, 30, 31, 40, 41, 42 };

// --- Chip selects, active low (outputs) -----------------------------------
// CS0 -> 0x8000-0x9FFF, CS1 -> 0xA000-0xBFFF,
// CS2 -> 0xC000-0xDFFF, CS3 -> 0xE000-0xFFFF
static const uint8_t cs_pins[4] = { 43, 44, 45, 8 };

// --- Cartridge presence ---------------------------------------------------
// Held low by the shield when a cartridge is seated. Set to -1 to fall back to
// detecting presence purely by reading the 0x55AA/0xAA55 header signature.
#define PIN_CART_DETECT   9

static bool pins_ready = false;

static void cart_gpio_init(void) {
    if (pins_ready) return;

    gpio_init(PIN_SR_DATA);  gpio_set_dir(PIN_SR_DATA,  GPIO_OUT);
    gpio_init(PIN_SR_CLOCK); gpio_set_dir(PIN_SR_CLOCK, GPIO_OUT);
    gpio_init(PIN_SR_LATCH); gpio_set_dir(PIN_SR_LATCH, GPIO_OUT);
    gpio_put(PIN_SR_DATA, 0);
    gpio_put(PIN_SR_CLOCK, 0);
    gpio_put(PIN_SR_LATCH, 0);

    for (int i = 0; i < 8; i++) {
        gpio_init(data_pins[i]);
        gpio_set_dir(data_pins[i], GPIO_IN);
        // RP2350 A2 erratum E9: the internal pull-down is too weak to hold a
        // high-impedance input low. Pull up instead and read the cartridge's
        // active drive, which is what the shield expects.
        gpio_pull_up(data_pins[i]);
    }

    for (int i = 0; i < 4; i++) {
        gpio_init(cs_pins[i]);
        gpio_set_dir(cs_pins[i], GPIO_OUT);
        gpio_put(cs_pins[i], 1);          // deselected
    }

#if PIN_CART_DETECT >= 0
    gpio_init(PIN_CART_DETECT);
    gpio_set_dir(PIN_CART_DETECT, GPIO_IN);
    gpio_pull_up(PIN_CART_DETECT);
#endif

    pins_ready = true;
}

// Clock a 15-bit address out to the shift register pair, MSB first.
static void set_address(uint16_t addr) {
    gpio_put(PIN_SR_LATCH, 0);
    for (int bit = 15; bit >= 0; bit--) {
        gpio_put(PIN_SR_DATA, (addr >> bit) & 1);
        gpio_put(PIN_SR_CLOCK, 1);
        asm volatile("nop; nop; nop; nop");
        gpio_put(PIN_SR_CLOCK, 0);
    }
    gpio_put(PIN_SR_LATCH, 1);
    asm volatile("nop; nop; nop; nop");
    gpio_put(PIN_SR_LATCH, 0);
}

static uint8_t read_data_bus(void) {
    uint32_t all = gpio_get_all();
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        if (all & (1u << data_pins[i])) v |= (uint8_t)(1 << i);
    return v;
}

// Read one byte from bank `bank` (0-3) at offset `off` within that 8 KB bank.
static uint8_t cart_read_byte(int bank, uint16_t off) {
    set_address(off & 0x1FFF);
    gpio_put(cs_pins[bank], 0);
    // 74HC595 propagation plus the ROM's access time (~200-450 ns on period
    // parts). Two microseconds is generous and still fast enough overall.
    sleep_us(2);
    uint8_t v = read_data_bus();
    gpio_put(cs_pins[bank], 1);
    return v;
}

// GPIO 20 and 21 are the codec's I2C bus as well as two of the cartridge data
// lines. cart_gpio_init() takes them over as inputs, so when no shield is
// fitted they must be handed back -- otherwise a probe that finds nothing
// leaves I2C dead for the rest of the run.
static void restore_i2c_pins(void) {
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_SDA);
    gpio_pull_up(PIN_I2C_SCL);
}

bool cart_present(void) {
    cart_gpio_init();

#if PIN_CART_DETECT >= 0
    sleep_ms(1);
    if (gpio_get(PIN_CART_DETECT)) {               // idles high; low = seated
        restore_i2c_pins();
        return false;
    }
#endif

    // Confirm with the header signature. Every ColecoVision cartridge starts
    // with 0xAA 0x55 (show the BIOS title screen) or 0x55 0xAA (skip it).
    uint8_t b0 = cart_read_byte(0, 0);
    uint8_t b1 = cart_read_byte(0, 1);
    const bool found = (b0 == 0xAA && b1 == 0x55) || (b0 == 0x55 && b1 == 0xAA);
    if (!found) restore_i2c_pins();
    return found;
}

uint32_t cart_read(uint8_t *dst, uint32_t max_len) {
    cart_gpio_init();

    uint32_t total = 0;
    for (int bank = 0; bank < 4; bank++) {
        // An unpopulated bank floats; with pull-ups that reads back as all
        // 0xFF, which is how we find the real cartridge size.
        bool empty = true;
        uint8_t probe[16];
        for (int i = 0; i < 16; i++) {
            probe[i] = cart_read_byte(bank, (uint16_t)(i * 0x100));
            if (probe[i] != 0xFF) empty = false;
        }
        if (empty && bank > 0) break;

        for (uint32_t off = 0; off < 0x2000; off++) {
            if (total >= max_len) return total;
            dst[total++] = cart_read_byte(bank, (uint16_t)off);
        }
    }
    return total;
}

#else  // !ENABLE_CART_READER

bool     cart_present(void) { return false; }
uint32_t cart_read(uint8_t *dst, uint32_t max_len) { (void)dst; (void)max_len; return 0; }

#endif
