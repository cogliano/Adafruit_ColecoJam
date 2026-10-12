// eeprom24.cpp -- 24C08 / 24C256 serial EEPROM
//
// An I2C target, driven one bus edge at a time because the games bit-bang the
// two lines through cartridge hotspots rather than through any peripheral.
//
// The sequences being followed are the ones in the datasheets:
//
//   byte write      START, device+W, address, data, STOP
//   random read     START, device+W, address, START, device+R, data..., STOP
//   current read    START, device+R, data..., STOP
//
// The device address byte is 1010 followed by three bits. On a 24C256 those
// three are chip-select pins, all grounded here, so it answers only to 0xA0.
// On a 24C08 the upper two of them carry address bits 9 and 8, so it answers
// to 0xA0, 0xA2, 0xA4 and 0xA6 -- which is also how a 1 KB part is addressed
// with a single address byte.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eeprom24.h"
#include "config.h"
#include <string.h>

namespace {

uint8_t *mem      = nullptr;
uint32_t mem_size = 0;
bool     dirty    = false;

bool scl = true, sda = true;      // as driven by the game
bool dev_sda = true;              // as driven by the chip; low = pulling down

enum Phase {
    IDLE,         // between a STOP and the next START
    DEV_ADDR,     // receiving the 1010xxx + R/W byte
    ADDR_HI,      // 24C256 only
    ADDR_LO,
    WRITE_DATA,
    READ_DATA,
};

Phase    phase   = IDLE;
uint8_t  shifter = 0;
int      bits    = 0;             // bits moved in the current byte
bool     acking  = false;         // in the ninth (acknowledge) clock
bool     acked   = false;         // the ack has been placed on the line
bool     ack_bit = true;          // what was sampled during the ninth clock
bool     reading = false;         // transfer direction
bool     got_addr_hi = false;
uint32_t addr    = 0;
uint8_t  dev_page = 0;            // 24C08: address bits from the device byte

inline bool is_24c08(void) { return mem_size <= EEPROM_24C08_BYTES; }

inline void begin_byte(void) { shifter = 0; bits = 0; acking = false; acked = false; }

// Page writes wrap within a page rather than running on into the next one.
// Both parts here use a 16-byte page, and a game that writes past the end of
// one relies on that wrap, so it has to be modelled.
inline void advance_write_addr(void) {
    const uint32_t page = 16u;
    const uint32_t base = addr & ~(page - 1u);
    addr = base | ((addr + 1u) & (page - 1u));
}

inline void advance_read_addr(void) {
    addr = (addr + 1u) % (mem_size ? mem_size : 1u);
}

} // namespace

void eeprom_attach(uint32_t size, uint8_t *buffer) {
    mem      = buffer;
    mem_size = buffer ? size : 0;
    if (mem_size) memset(mem, 0xFF, mem_size);   // erased state of a real part
    eeprom_reset();
    dirty = false;
}

void eeprom_reset(void) {
    phase = IDLE;
    scl = sda = dev_sda = true;
    shifter = 0; bits = 0; acking = false; acked = false;
    ack_bit = true; reading = false;
    got_addr_hi = false; addr = 0; dev_page = 0;
}

uint8_t *eeprom_data(void)      { return mem; }
uint32_t eeprom_size(void)      { return mem_size; }
bool     eeprom_dirty(void)     { return dirty; }
void     eeprom_clear_dirty(void) { dirty = false; }

bool eeprom_get_sda(void) {
    // Open drain: either side can pull the line down.
    return sda && dev_sda;
}

void eeprom_set_sda(bool level) {
    if (!mem_size) { sda = level; return; }

    // START and STOP are defined by SDA moving while SCL is high; every other
    // SDA change happens with the clock low and is just data being set up.
    if (scl && level != sda) {
        if (!level) {
            // Falling: START. Also a repeated START, which is how a random
            // read turns around without releasing the bus.
            phase   = DEV_ADDR;
            reading = false;
            got_addr_hi = false;
            dev_sda = true;
            begin_byte();
        } else {
            phase   = IDLE;
            dev_sda = true;
        }
    }
    sda = level;
}

void eeprom_set_scl(bool level) {
    if (!mem_size) { scl = level; return; }

    const bool rising  = level && !scl;
    const bool falling = !level && scl;
    scl = level;

    // Acknowledgement takes a WHOLE NINTH CLOCK, which is the thing that is
    // easy to get wrong: the receiver pulls the line down on the eighth
    // falling edge, the sender samples it while the ninth clock is high, and
    // the line is released on the ninth falling edge. Treating the ack as part
    // of the eighth clock makes the ninth pulse look like a data bit and every
    // byte after the first lands a bit out.
    if (rising) {
        if (acking) {
            // Ninth clock high: for a read this is where the master's
            // ack-or-not is readable. For a write it is our own ack.
            ack_bit = eeprom_get_sda();
            return;
        }
        if (phase == IDLE) return;

        if (phase == READ_DATA) {
            if (++bits == 8) acking = true;      // chip has sent a byte
        } else {
            shifter = (uint8_t)((shifter << 1) | (eeprom_get_sda() ? 1 : 0));
            if (++bits == 8) acking = true;      // chip has received one
        }
        return;
    }

    if (!falling) return;

    if (acking && !acked) {
        // Eighth falling edge: act on the byte and put the ack on the line.
        acked = true;

        switch (phase) {
        case DEV_ADDR:
            if ((shifter & 0xF0) != 0xA0) {      // not this chip: stay quiet
                phase = IDLE; dev_sda = true;
                acking = false; acked = false;
                return;
            }
            reading  = (shifter & 1) != 0;
            dev_page = (uint8_t)((shifter >> 1) & 0x07);
            dev_sda  = false;                    // acknowledge
            break;

        case ADDR_HI:
            addr = (uint32_t)shifter << 8;
            got_addr_hi = true;
            dev_sda = false;
            break;

        case ADDR_LO:
            if (is_24c08()) addr = (uint32_t)((dev_page & 0x03) << 8) | shifter;
            else            addr = (got_addr_hi ? (addr & 0xFF00) : 0u) | shifter;
            dev_sda = false;
            break;

        case WRITE_DATA:
            mem[addr % mem_size] = shifter;
            dirty = true;
            advance_write_addr();
            dev_sda = false;
            break;

        case READ_DATA:
            dev_sda = true;                      // release, so the master can
            break;                               // drive its ack
        default:
            break;
        }
        return;
    }

    if (acking && acked) {
        // Ninth falling edge: the ack is over. Move to whatever comes next.
        acking = false;
        acked  = false;

        switch (phase) {
        case DEV_ADDR:
            if (reading) {
                if (is_24c08())
                    addr = (uint32_t)((dev_page & 0x03) << 8) | (addr & 0xFF);
                phase = READ_DATA;
                begin_byte();
                dev_sda = (mem[addr % mem_size] & 0x80) != 0;
            } else {
                phase = is_24c08() ? ADDR_LO : ADDR_HI;
                begin_byte();
                dev_sda = true;
            }
            return;

        case ADDR_HI:
            phase = ADDR_LO;  begin_byte(); dev_sda = true; return;
        case ADDR_LO:
            phase = WRITE_DATA; begin_byte(); dev_sda = true; return;
        case WRITE_DATA:
            begin_byte(); dev_sda = true; return;

        case READ_DATA:
            if (ack_bit) {                       // high = NACK, master is done
                phase = IDLE; dev_sda = true;
                return;
            }
            advance_read_addr();
            begin_byte();
            dev_sda = (mem[addr % mem_size] & 0x80) != 0;
            return;

        default:
            return;
        }
    }

    // Between bits of a byte the chip is sending: present the next one.
    if (phase == READ_DATA) dev_sda = (mem[addr % mem_size] & (0x80 >> bits)) != 0;
}
