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
#include "hardware/uart.h"
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

    // Same for the debug console. cart_gpio_init() took every shield pin as
    // plain GPIO, and the console's pins are shield pins too: GPIO 8/9 by
    // default, 44/45 if moved (PICO_DEFAULT_UART_TX/RX_PIN in CMakeLists.txt).
    // Without this, all serial output stops at the cartridge probe on every
    // boot. The function select is chosen exactly as stdio_uart_init() does.
#if defined(PICO_DEFAULT_UART_TX_PIN) && defined(PICO_DEFAULT_UART_RX_PIN)
    gpio_set_function(PICO_DEFAULT_UART_TX_PIN,
                      UART_FUNCSEL_NUM(uart_default, PICO_DEFAULT_UART_TX_PIN));
    gpio_set_function(PICO_DEFAULT_UART_RX_PIN,
                      UART_FUNCSEL_NUM(uart_default, PICO_DEFAULT_UART_RX_PIN));
#endif

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

#if ENABLE_MEGACART
// --- MegaCart dumping ------------------------------------------------------
//
// A MegaCart does its banking on the cartridge, so reading one means driving
// the bank select ourselves:
//
//   0x8000-0xBFFF   the last bank, fixed          CS0 and CS1
//   0xC000-0xFFFF   the switchable bank           CS2 and CS3
//   0xFFC0 + n      selecting bank n is a READ    CS3 at offset 0x1FC0 + n
//
// Note the consequence of that last line: the select addresses live inside the
// switchable window, so the final 64 bytes of a bank cannot be read without
// changing bank. They are dumped as 0xFF for every bank except the last, which
// is readable in full through the fixed window. No game can place code there
// -- it could not execute it without switching away mid-instruction.

#define BANK_BYTES   0x4000u
#define SEL_BASE     0x1FC0u          // 0xFFC0 within the CS3 window
#define SEL_COUNT    0x40u            // 64 select addresses

// Point the switchable window at bank n.
static inline void cart_select_bank(uint32_t n) {
    (void)cart_read_byte(3, (uint16_t)(SEL_BASE + (n & (SEL_COUNT - 1))));
}

// Checksum a bank, avoiding the select region entirely.
static uint32_t cart_bank_hash(uint32_t bank) {
    cart_select_bank(bank);
    uint32_t h = 2166136261u;
    for (uint16_t off = 0; off < 0x2000; off += 64) {       // CS2 half
        h = (h ^ cart_read_byte(2, off)) * 16777619u;
    }
    for (uint16_t off = 0; off < 0x1F00; off += 64) {       // CS3 half, clear
        h = (h ^ cart_read_byte(3, off)) * 16777619u;       // of the selects
    }
    return h;
}

// How many 16 KB banks the cartridge has, or 0 if it is not bank-switched.
//
// Selecting a bank beyond the real count wraps, because the cartridge masks
// the select with its own bank count. So the first count at which bank c reads
// back the same as bank 0 is the real count. If bank 1 already matches bank 0
// there is no banking hardware at all.
static uint32_t cart_detect_banks(void) {
    const uint32_t h0 = cart_bank_hash(0);
    if (cart_bank_hash(1) == h0) return 0;                  // not banked

    for (uint32_t c = 2; c <= 64; c <<= 1) {
        if (cart_bank_hash(c) == h0) return c;
    }
    return 64;                                              // the format's max
}

uint32_t cart_read_banked(uint8_t *dst, uint32_t max_len,
                          cart_progress_fn progress) {
    cart_gpio_init();

    const uint32_t banks = cart_detect_banks();
    if (banks == 0) {
        // Not bank-switched. Release the pins rather than leaving them driven
        // for cart_read() to re-initialise underneath itself.
        cart_gpio_release();
        return 0;
    }

    const uint32_t total = banks * BANK_BYTES;
    if (total > max_len) return 0;

    for (uint32_t b = 0; b < banks; b++) {
        uint8_t *out = dst + b * BANK_BYTES;

        if (b == banks - 1) {
            // The last bank is permanently at 0x8000, so it can be read
            // whole -- including the bytes that are unreachable elsewhere.
            for (uint16_t off = 0; off < 0x2000; off++) out[off] = cart_read_byte(0, off);
            for (uint16_t off = 0; off < 0x2000; off++) out[0x2000 + off] = cart_read_byte(1, off);
        } else {
            cart_select_bank(b);
            for (uint16_t off = 0; off < 0x2000; off++) out[off] = cart_read_byte(2, off);
            for (uint16_t off = 0; off < 0x1FC0; off++) out[0x2000 + off] = cart_read_byte(3, off);
            // The select region: unreadable for this bank.
            for (uint16_t off = 0x1FC0; off < 0x2000; off++) out[0x2000 + off] = 0xFF;
        }

        if (progress) progress((b + 1) * BANK_BYTES, total);
    }

    cart_gpio_release();
    return total;
}
#endif  // ENABLE_MEGACART

// What the last read made of the cartridge, so a wrong size can be looked at
// rather than inferred. banks[] is one character per chip select in the order
// they were read: P populated, B uniform but 0xFF/0x00 so treated as a blank
// part of a real chip, U uniform and floating so treated as absent, '-' not
// reached.
CartReadStatus cart_status = { 0, 0, { '-', '-', '-', '-', 0 }, 0 };

#if CART_TRIM_MIRROR
// Shorten the last 8 KB if the chip there is smaller than its window.
//
// Chip selects are 8 KB wide, so a dump is always a multiple of 8 KB, but the
// final chip need not be that big. A 4 KB chip has A12 unconnected and answers
// to both halves of its window, so the second half of the read is the first
// half again -- which is why a 20 KB cartridge comes back as 24 KB.
//
// Halving repeatedly also catches 2 KB and 1 KB chips. It stops at 1 KB: below
// that the chance of a genuine ROM happening to repeat outweighs the chance of
// a cartridge actually using a chip that small.
//
// Only the LAST bank is considered. An earlier bank reading as mirrored would
// mean something stranger than a small chip, and trimming in the middle would
// corrupt everything after it.
static uint32_t trim_mirrored_tail(const uint8_t *dst, uint32_t total) {
    if (total < 0x2000) return total;

    const uint8_t *bank = dst + total - 0x2000;
    uint32_t size = 0x2000;
    while (size > 0x400 &&
           memcmp(bank, bank + size / 2, size / 2) == 0) {
        size /= 2;
    }
    return total - 0x2000 + size;
}
#endif

#if CART_TRIM_BLANK
// Shorten the last 8 KB if its upper part was never programmed.
//
// An erased tail is one repeated byte, 0xFF on an EPROM or 0x00 on a mask ROM.
// Only those two count: any other repeated value is data, and a bank that
// genuinely held, say, 4 KB of 0x20 would be real content.
//
// Last bank only, for the same reason as the mirror trim -- a blank stretch in
// the middle of a cartridge is part of it, and cutting there would move
// everything after.
static uint32_t trim_blank_tail(const uint8_t *dst, uint32_t total) {
    if (total < 0x2000) return total;

    const uint8_t *bank = dst + total - 0x2000;
    uint32_t size = 0x2000;
    while (size > 0x400) {
        const uint8_t *upper = bank + size / 2;
        const uint8_t  fill  = upper[0];
        if (fill != 0xFF && fill != 0x00) break;

        bool blank = true;
        for (uint32_t i = 1; i < size / 2 && blank; i++)
            if (upper[i] != fill) blank = false;
        if (!blank) break;

        size /= 2;
    }
    return total - 0x2000 + size;
}
#endif

// Is this chip select empty?
//
// Asking whether the data varies is not enough. An undriven bus settles to
// 0xC0 here, but RP2350-E9 means a pad with a pull-down can hold the level it
// was last driven to, so the first reads of an empty socket return whatever
// the PREVIOUS bank put on the bus. A dump of an empty third socket looks like
// "40 40 C0 C0 C0..." -- varying, and so mistaken for a chip.
//
// What no ROM does is return a different byte for the same address depending
// on what was read just before it. A floating bus does exactly that, and it is
// a direct test rather than an inference from the data's shape.
//
// Each address is read twice: once just after an address in a known-populated
// bank holding one value, once just after an address holding a different one.
// A real chip gives the same byte both times. A floating bus follows whichever
// value was driven last.
static bool bank_is_empty(int bank, int probe_bank) {
    // Two addresses in the probe bank whose contents differ, to drive the bus
    // to two different states.
    const uint8_t va = cart_read_byte(probe_bank, 0x0000);
    uint16_t ob = 0;
    uint8_t  vb = va;
    for (uint16_t o = 1; o < 0x2000 && vb == va; o = (uint16_t)(o + 37)) {
        vb = cart_read_byte(probe_bank, o);
        ob = o;
    }
    if (vb == va) {
        // The probe bank reads the same everywhere, so the bus cannot be
        // driven to two states and this test cannot say anything. Fall back
        // to the shape of the data.
        return false;
    }

    for (int j = 0; j < 24; j++) {
        const uint16_t off = (uint16_t)((j * 0x0107 + 3) & 0x1FFF);

        cart_read_byte(probe_bank, 0x0000);          // drive the bus to va
        const uint8_t x = cart_read_byte(bank, off);

        cart_read_byte(probe_bank, ob);              // and now to vb
        const uint8_t y = cart_read_byte(bank, off);

        if (x != y) return true;                     // follows history: empty
    }
    return false;
}

uint32_t cart_read(uint8_t *dst, uint32_t max_len) {
    cart_gpio_init();

    // Cleared every time. Leaving it from the previous read made a short dump
    // report banks it had not looked at, which hid the real fault.
    cart_status.raw = cart_status.trimmed = 0;
    cart_status.uniform_value = 0;
    for (int i = 0; i < 4; i++) cart_status.banks[i] = '-';

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
        // Re-test anything that looks uniform before believing it.
        //
        // A missing chip reads the same value at every address, but so does a
        // bus that has not settled -- and a stale value sampled 32 times is
        // indistinguishable from an empty socket. That is what made the same
        // cartridge read as 24 KB one run and 16 KB the next with no reboot
        // between them.
        //
        // An absent chip is uniform every time, under any timing. A settling
        // problem is not. So a uniform result is retried with a pause and a
        // different set of addresses, and any variation at all means the chip
        // is there.
        bool    uniform      = true;
        uint8_t first_sample = 0;
        for (int attempt = 0; attempt < CART_EMPTY_RETRIES && uniform; attempt++) {
            if (attempt) sleep_us(200);           // let the bus settle

            // Different strides each time, so a ROM that happens to repeat at
            // one stride is not mistaken for an empty socket twice over.
            const uint16_t stride = (uint16_t)(0x0107 + attempt * 0x0053);
            first_sample = cart_read_byte(bank, (uint16_t)(attempt * 7));
            for (int j = 1; j < 48 && uniform; j++) {
                const uint16_t off = (uint16_t)((j * stride) & 0x1FFF);
                if (cart_read_byte(bank, off) != first_sample) uniform = false;
            }
        }

        // A uniform bank reading 0xFF or 0x00 is a real chip with a blank
        // region: an empty socket floats to 0xC0 here, never to those.
        if (uniform) cart_status.uniform_value = first_sample;

        // Two independent signs of an empty socket, because each catches a
        // case the other misses.
        //
        //   floating  the bus follows what was read before it -- catches a
        //             socket whose first bytes are still latched from the
        //             previous bank, which looks like varying data
        //   uniform   every sample the same, and not an erased 0xFF/0x00 --
        //             catches a socket that has settled to 0xC0 by the time
        //             it is sampled
        const bool blank_rom = (first_sample == 0xFF || first_sample == 0x00);
        const bool floating  = (i > 0) && bank_is_empty(bank, first);

        if (i > 0 && (floating || (uniform && !blank_rom))) break;

        cart_status.banks[i] = floating ? 'F'
                             : uniform  ? (blank_rom ? 'B' : 'U')
                                        : 'P';

        for (uint32_t off = 0; off < 0x2000; off++) {
            if (total >= max_len) { cart_gpio_release(); return total; }
            dst[total++] = cart_read_byte(bank, (uint16_t)off);
        }
    }

    cart_status.raw = total;
#if CART_TRIM_MIRROR
    total = trim_mirrored_tail(dst, total);
#endif
#if CART_TRIM_BLANK
    total = trim_blank_tail(dst, total);
#endif
    cart_status.trimmed = total;

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
#if ENABLE_MEGACART
uint32_t cart_read_banked(uint8_t *, uint32_t, cart_progress_fn) { return 0; }
#endif

#endif
