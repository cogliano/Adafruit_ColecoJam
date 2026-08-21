// z80.h -- Zilog Z80A / NEC uPD780C CPU core
//
// Part of Adafruit_ColecoJam. Structure and behaviour follow the Gearcoleco
// (https://github.com/drhelius/Gearcoleco) Z80 core, reimplemented here as a
// flat C-style interpreter so that the hot loop fits comfortably in RP2350
// SRAM/XIP cache.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Bus callbacks. These are provided by coleco.cpp. They are plain functions
// rather than std::function/virtuals: on Cortex-M33 a direct call is roughly
// 3x cheaper than an indirect one and this is the single hottest path.
// ---------------------------------------------------------------------------
uint8_t cv_bus_read(uint16_t addr);
void    cv_bus_write(uint16_t addr, uint8_t value);
uint8_t cv_bus_in(uint16_t port);
void    cv_bus_out(uint16_t port, uint8_t value);

// Flag bits.
enum {
    FLAG_C = 0x01,   // Carry
    FLAG_N = 0x02,   // Add/Subtract
    FLAG_P = 0x04,   // Parity / Overflow
    FLAG_X = 0x08,   // Undocumented copy of bit 3
    FLAG_H = 0x10,   // Half carry
    FLAG_Y = 0x20,   // Undocumented copy of bit 5
    FLAG_Z = 0x40,   // Zero
    FLAG_S = 0x80,   // Sign
};

union Z80Reg {
    uint16_t w;
    struct { uint8_t l, h; } b;   // little-endian target (RP2350 is LE)
};

struct Z80 {
    Z80Reg af, bc, de, hl;
    Z80Reg af2, bc2, de2, hl2;    // shadow set
    Z80Reg ix, iy, sp, pc;
    Z80Reg wz;                    // internal MEMPTR

    uint8_t i;                    // interrupt vector
    uint8_t r;                    // refresh (bit 7 held separately)
    uint8_t r7;

    bool iff1, iff2;
    bool halted;
    bool ei_pending;              // EI delays interrupt acceptance one insn
    uint8_t im;                   // interrupt mode 0/1/2

    bool nmi_pending;
    bool int_pending;
    uint8_t int_vector;

    int32_t cycles;               // T-states consumed by the last step()
};

extern Z80 z80;

void z80_init(void);
void z80_reset(void);

// Execute exactly one instruction (plus any interrupt acceptance that happens
// first). Returns the number of T-states consumed.
int  z80_step(void);

// Run until at least `tstates` T-states have elapsed. Returns the actual number
// run, which may overshoot by up to one instruction.
int  z80_run(int tstates);

void z80_set_nmi(void);
void z80_set_int(bool asserted, uint8_t vector);
