// z80.cpp -- Z80A interpreter
// SPDX-License-Identifier: GPL-3.0-or-later

#include "z80.h"

#ifdef PICO_ON_DEVICE
#include "pico.h"
#define HOT __not_in_flash_func
#else
#define HOT(x) x
#endif

Z80 z80;

// ---------------------------------------------------------------------------
// Lookup tables
// ---------------------------------------------------------------------------
static uint8_t sz53_tbl[256];    // S, Z, Y, X for a byte
static uint8_t sz53p_tbl[256];   // ... plus parity
static uint8_t parity_tbl[256];

static void build_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        parity_tbl[i] = p ? 0 : FLAG_P;

        uint8_t sz = (uint8_t)(i & (FLAG_S | FLAG_Y | FLAG_X));
        if (i == 0) sz |= FLAG_Z;
        sz53_tbl[i]  = sz;
        sz53p_tbl[i] = sz | parity_tbl[i];
    }
}

// ---------------------------------------------------------------------------
// Shorthand
// ---------------------------------------------------------------------------
#define A   z80.af.b.h
#define F   z80.af.b.l
#define B   z80.bc.b.h
#define C   z80.bc.b.l
#define D   z80.de.b.h
#define E   z80.de.b.l
#define H   z80.hl.b.h
#define L   z80.hl.b.l
#define AF  z80.af.w
#define BC  z80.bc.w
#define DE  z80.de.w
#define HL  z80.hl.w
#define SP  z80.sp.w
#define PC  z80.pc.w
#define WZ  z80.wz.w

#define SETF(m)   (F |= (m))
#define CLRF(m)   (F &= (uint8_t)~(m))
#define TSTF(m)   (F & (m))

static inline uint8_t rd8(uint16_t a)              { return cv_bus_read(a); }
static inline void    wr8(uint16_t a, uint8_t v)   { cv_bus_write(a, v); }

static inline uint8_t fetch8(void)  { return rd8(PC++); }
static inline uint16_t fetch16(void) {
    uint16_t lo = fetch8();
    return (uint16_t)(lo | (fetch8() << 8));
}
static inline uint16_t rd16(uint16_t a) {
    return (uint16_t)(rd8(a) | (rd8((uint16_t)(a + 1)) << 8));
}
static inline void wr16(uint16_t a, uint16_t v) {
    wr8(a, (uint8_t)v);
    wr8((uint16_t)(a + 1), (uint8_t)(v >> 8));
}
static inline void push16(uint16_t v) {
    SP -= 2;
    wr8((uint16_t)(SP + 1), (uint8_t)(v >> 8));
    wr8(SP, (uint8_t)v);
}
static inline uint16_t pop16(void) {
    uint16_t v = rd16(SP);
    SP += 2;
    return v;
}

// R register: bit 7 is not incremented by the refresh counter.
static inline void bump_r(void) { z80.r = (uint8_t)((z80.r + 1) & 0x7F); }
static inline uint8_t get_r(void) { return (uint8_t)((z80.r & 0x7F) | z80.r7); }

// ---------------------------------------------------------------------------
// ALU
// ---------------------------------------------------------------------------
static inline void alu_add(uint8_t v) {
    uint16_t r = (uint16_t)(A + v);
    uint8_t  h = (uint8_t)((A ^ v ^ r) & FLAG_H);
    F = (uint8_t)(sz53_tbl[r & 0xFF] | h | (r > 0xFF ? FLAG_C : 0) |
                  ((((A ^ ~v) & (A ^ r)) & 0x80) ? FLAG_P : 0));
    A = (uint8_t)r;
}
static inline void alu_adc(uint8_t v) {
    uint8_t  c = (uint8_t)(F & FLAG_C);
    uint16_t r = (uint16_t)(A + v + c);
    uint8_t  h = (uint8_t)((A ^ v ^ r) & FLAG_H);
    F = (uint8_t)(sz53_tbl[r & 0xFF] | h | (r > 0xFF ? FLAG_C : 0) |
                  ((((A ^ ~v) & (A ^ r)) & 0x80) ? FLAG_P : 0));
    A = (uint8_t)r;
}
static inline void alu_sub(uint8_t v) {
    uint16_t r = (uint16_t)(A - v);
    uint8_t  h = (uint8_t)((A ^ v ^ r) & FLAG_H);
    F = (uint8_t)(sz53_tbl[r & 0xFF] | h | FLAG_N | (r > 0xFF ? FLAG_C : 0) |
                  ((((A ^ v) & (A ^ r)) & 0x80) ? FLAG_P : 0));
    A = (uint8_t)r;
}
static inline void alu_sbc(uint8_t v) {
    uint8_t  c = (uint8_t)(F & FLAG_C);
    uint16_t r = (uint16_t)(A - v - c);
    uint8_t  h = (uint8_t)((A ^ v ^ r) & FLAG_H);
    F = (uint8_t)(sz53_tbl[r & 0xFF] | h | FLAG_N | (r > 0xFF ? FLAG_C : 0) |
                  ((((A ^ v) & (A ^ r)) & 0x80) ? FLAG_P : 0));
    A = (uint8_t)r;
}
static inline void alu_and(uint8_t v) { A &= v; F = (uint8_t)(sz53p_tbl[A] | FLAG_H); }
static inline void alu_xor(uint8_t v) { A ^= v; F = sz53p_tbl[A]; }
static inline void alu_or (uint8_t v) { A |= v; F = sz53p_tbl[A]; }
static inline void alu_cp (uint8_t v) {
    uint16_t r = (uint16_t)(A - v);
    uint8_t  h = (uint8_t)((A ^ v ^ r) & FLAG_H);
    // CP takes the undocumented Y/X flags from the operand, not the result.
    F = (uint8_t)((sz53_tbl[r & 0xFF] & (FLAG_S | FLAG_Z)) |
                  (v & (FLAG_Y | FLAG_X)) | h | FLAG_N |
                  (r > 0xFF ? FLAG_C : 0) |
                  ((((A ^ v) & (A ^ r)) & 0x80) ? FLAG_P : 0));
}

static inline uint8_t alu_inc(uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    F = (uint8_t)((F & FLAG_C) | sz53_tbl[r] |
                  ((r & 0x0F) == 0x00 ? FLAG_H : 0) |
                  (r == 0x80 ? FLAG_P : 0));
    return r;
}
static inline uint8_t alu_dec(uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    F = (uint8_t)((F & FLAG_C) | sz53_tbl[r] | FLAG_N |
                  ((r & 0x0F) == 0x0F ? FLAG_H : 0) |
                  (r == 0x7F ? FLAG_P : 0));
    return r;
}

static inline void alu_add16(Z80Reg *dst, uint16_t v) {
    uint32_t r = (uint32_t)dst->w + v;
    WZ = (uint16_t)(dst->w + 1);
    F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) |
                  (((dst->w ^ v ^ r) >> 8) & FLAG_H) |
                  ((r >> 16) ? FLAG_C : 0) |
                  ((r >> 8) & (FLAG_Y | FLAG_X)));
    dst->w = (uint16_t)r;
}
static inline void alu_adc16(uint16_t v) {
    uint8_t  c = (uint8_t)(F & FLAG_C);
    uint32_t r = (uint32_t)HL + v + c;
    WZ = (uint16_t)(HL + 1);
    F = (uint8_t)((((HL ^ v ^ r) >> 8) & FLAG_H) |
                  ((r >> 16) ? FLAG_C : 0) |
                  (((r & 0xFFFF) == 0) ? FLAG_Z : 0) |
                  ((r >> 8) & (FLAG_S | FLAG_Y | FLAG_X)) |
                  ((((HL ^ ~v) & (HL ^ r)) & 0x8000) ? FLAG_P : 0));
    HL = (uint16_t)r;
}
static inline void alu_sbc16(uint16_t v) {
    uint8_t  c = (uint8_t)(F & FLAG_C);
    uint32_t r = (uint32_t)HL - v - c;
    WZ = (uint16_t)(HL + 1);
    F = (uint8_t)((((HL ^ v ^ r) >> 8) & FLAG_H) | FLAG_N |
                  ((r >> 16) ? FLAG_C : 0) |
                  (((r & 0xFFFF) == 0) ? FLAG_Z : 0) |
                  ((r >> 8) & (FLAG_S | FLAG_Y | FLAG_X)) |
                  ((((HL ^ v) & (HL ^ r)) & 0x8000) ? FLAG_P : 0));
    HL = (uint16_t)r;
}

// ---------------------------------------------------------------------------
// Rotates / shifts
// ---------------------------------------------------------------------------
static inline uint8_t op_rlc(uint8_t v) {
    uint8_t c = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | c);
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_rrc(uint8_t v) {
    uint8_t c = (uint8_t)(v & 1);
    v = (uint8_t)((v >> 1) | (c << 7));
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_rl(uint8_t v) {
    uint8_t c = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | (F & FLAG_C));
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_rr(uint8_t v) {
    uint8_t c = (uint8_t)(v & 1);
    v = (uint8_t)((v >> 1) | ((F & FLAG_C) << 7));
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_sla(uint8_t v) {
    uint8_t c = (uint8_t)(v >> 7);
    v = (uint8_t)(v << 1);
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_sra(uint8_t v) {
    uint8_t c = (uint8_t)(v & 1);
    v = (uint8_t)((v >> 1) | (v & 0x80));
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_sll(uint8_t v) {      // undocumented
    uint8_t c = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | 1);
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline uint8_t op_srl(uint8_t v) {
    uint8_t c = (uint8_t)(v & 1);
    v = (uint8_t)(v >> 1);
    F = (uint8_t)(sz53p_tbl[v] | c);
    return v;
}
static inline void op_bit(uint8_t v, int bit) {
    uint8_t r = (uint8_t)(v & (1 << bit));
    F = (uint8_t)((F & FLAG_C) | FLAG_H |
                  (r ? (r & FLAG_S) : (FLAG_Z | FLAG_P)) |
                  (v & (FLAG_Y | FLAG_X)));
}
// BIT n,(HL) and BIT n,(IX+d) take Y/X from the internal address latch.
static inline void op_bit_mem(uint8_t v, int bit, uint16_t addr) {
    uint8_t r = (uint8_t)(v & (1 << bit));
    F = (uint8_t)((F & FLAG_C) | FLAG_H |
                  (r ? (r & FLAG_S) : (FLAG_Z | FLAG_P)) |
                  ((addr >> 8) & (FLAG_Y | FLAG_X)));
}

static inline void op_daa(void) {
    uint8_t corr = 0, c = (uint8_t)(F & FLAG_C);
    if ((F & FLAG_H) || (A & 0x0F) > 9) corr |= 0x06;
    if (c || A > 0x99) { corr |= 0x60; c = FLAG_C; }
    uint8_t old = A;
    if (F & FLAG_N) A = (uint8_t)(A - corr); else A = (uint8_t)(A + corr);
    F = (uint8_t)(sz53p_tbl[A] | (F & FLAG_N) | c |
                  ((old ^ A) & FLAG_H));
}

static inline void op_rld(void) {
    uint8_t m = rd8(HL);
    uint8_t lo = (uint8_t)(A & 0x0F);
    A = (uint8_t)((A & 0xF0) | (m >> 4));
    wr8(HL, (uint8_t)((m << 4) | lo));
    WZ = (uint16_t)(HL + 1);
    F = (uint8_t)((F & FLAG_C) | sz53p_tbl[A]);
}
static inline void op_rrd(void) {
    uint8_t m = rd8(HL);
    uint8_t lo = (uint8_t)(A & 0x0F);
    A = (uint8_t)((A & 0xF0) | (m & 0x0F));
    wr8(HL, (uint8_t)((m >> 4) | (lo << 4)));
    WZ = (uint16_t)(HL + 1);
    F = (uint8_t)((F & FLAG_C) | sz53p_tbl[A]);
}

static inline uint8_t op_in_c(void) {
    uint8_t v = cv_bus_in(BC);
    WZ = (uint16_t)(BC + 1);
    F = (uint8_t)((F & FLAG_C) | sz53p_tbl[v]);
    return v;
}

// ---------------------------------------------------------------------------
// Cycle tables (T-states, base cost; conditional extras added inline)
// ---------------------------------------------------------------------------
static const uint8_t cyc_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};
static const uint8_t cyc_ed[256] = {
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    12,12,15,20, 8,14, 8, 9,12,12,15,20, 8,14, 8, 9,
    12,12,15,20, 8,14, 8, 9,12,12,15,20, 8,14, 8, 9,
    12,12,15,20, 8,14, 8,18,12,12,15,20, 8,14, 8,18,
    12,12,15,20, 8,14, 8, 8,12,12,15,20, 8,14, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    16,16,16,16, 8, 8, 8, 8,16,16,16,16, 8, 8, 8, 8,
    16,16,16,16, 8, 8, 8, 8,16,16,16,16, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};

static void exec_cb(void);
static void exec_ed(void);
static void exec_ddfd(Z80Reg *ir);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void z80_init(void) {
    build_tables();
    z80_reset();
}

void z80_reset(void) {
    AF = 0xFFFF; BC = 0; DE = 0; HL = 0;
    z80.af2.w = 0xFFFF; z80.bc2.w = 0; z80.de2.w = 0; z80.hl2.w = 0;
    z80.ix.w = 0xFFFF; z80.iy.w = 0xFFFF;
    SP = 0xFFFF; PC = 0x0000; WZ = 0;
    z80.i = 0; z80.r = 0; z80.r7 = 0;
    z80.iff1 = z80.iff2 = false;
    z80.halted = false;
    z80.ei_pending = false;
    z80.im = 0;
    z80.nmi_pending = false;
    z80.int_pending = false;
    z80.int_vector = 0xFF;
    z80.cycles = 0;
}

void z80_set_nmi(void) { z80.nmi_pending = true; }

void z80_set_int(bool asserted, uint8_t vector) {
    z80.int_pending = asserted;
    z80.int_vector = vector;
}

// ---------------------------------------------------------------------------
// Interrupt acceptance
// ---------------------------------------------------------------------------
static int accept_nmi(void) {
    z80.nmi_pending = false;
    if (z80.halted) { z80.halted = false; PC++; }
    z80.iff2 = z80.iff1;
    z80.iff1 = false;
    bump_r();
    push16(PC);
    PC = 0x0066;
    WZ = 0x0066;
    return 11;
}

static int accept_int(void) {
    if (z80.halted) { z80.halted = false; PC++; }
    z80.iff1 = z80.iff2 = false;
    bump_r();
    switch (z80.im) {
    case 0:
        // Mode 0 executes the byte on the bus. Nothing on a ColecoVision
        // supplies one, so treat the usual RST 38h default.
        push16(PC); PC = 0x0038; WZ = PC;
        return 13;
    case 1:
        push16(PC); PC = 0x0038; WZ = PC;
        return 13;
    default: {
        push16(PC);
        uint16_t v = (uint16_t)((z80.i << 8) | z80.int_vector);
        PC = rd16(v);
        WZ = PC;
        return 19;
    }
    }
}

// ---------------------------------------------------------------------------
// Main dispatch
// ---------------------------------------------------------------------------
int HOT(z80_step)(void) {
    z80.cycles = 0;

    if (z80.nmi_pending) {
        z80.cycles = accept_nmi();
        return z80.cycles;
    }
    if (z80.int_pending && z80.iff1 && !z80.ei_pending) {
        z80.cycles = accept_int();
        return z80.cycles;
    }
    z80.ei_pending = false;

    if (z80.halted) { z80.cycles = 4; bump_r(); return 4; }

    uint8_t op = fetch8();
    bump_r();
    z80.cycles = cyc_main[op];

    switch (op) {
    case 0x00: break;                                            // NOP
    case 0x01: BC = fetch16(); break;                            // LD BC,nn
    case 0x02: wr8(BC, A); WZ = (uint16_t)((A << 8) | ((BC + 1) & 0xFF)); break;
    case 0x03: BC++; break;
    case 0x04: B = alu_inc(B); break;
    case 0x05: B = alu_dec(B); break;
    case 0x06: B = fetch8(); break;
    case 0x07: {                                                 // RLCA
        uint8_t c = (uint8_t)(A >> 7);
        A = (uint8_t)((A << 1) | c);
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) | c | (A & (FLAG_Y | FLAG_X)));
        break; }
    case 0x08: { uint16_t t = AF; AF = z80.af2.w; z80.af2.w = t; break; }
    case 0x09: alu_add16(&z80.hl, BC); break;
    case 0x0A: A = rd8(BC); WZ = (uint16_t)(BC + 1); break;
    case 0x0B: BC--; break;
    case 0x0C: C = alu_inc(C); break;
    case 0x0D: C = alu_dec(C); break;
    case 0x0E: C = fetch8(); break;
    case 0x0F: {                                                 // RRCA
        uint8_t c = (uint8_t)(A & 1);
        A = (uint8_t)((A >> 1) | (c << 7));
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) | c | (A & (FLAG_Y | FLAG_X)));
        break; }

    case 0x10: {                                                 // DJNZ d
        int8_t d = (int8_t)fetch8();
        if (--B) { PC = (uint16_t)(PC + d); WZ = PC; z80.cycles += 5; }
        break; }
    case 0x11: DE = fetch16(); break;
    case 0x12: wr8(DE, A); WZ = (uint16_t)((A << 8) | ((DE + 1) & 0xFF)); break;
    case 0x13: DE++; break;
    case 0x14: D = alu_inc(D); break;
    case 0x15: D = alu_dec(D); break;
    case 0x16: D = fetch8(); break;
    case 0x17: {                                                 // RLA
        uint8_t c = (uint8_t)(A >> 7);
        A = (uint8_t)((A << 1) | (F & FLAG_C));
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) | c | (A & (FLAG_Y | FLAG_X)));
        break; }
    case 0x18: { int8_t d = (int8_t)fetch8(); PC = (uint16_t)(PC + d); WZ = PC; break; }
    case 0x19: alu_add16(&z80.hl, DE); break;
    case 0x1A: A = rd8(DE); WZ = (uint16_t)(DE + 1); break;
    case 0x1B: DE--; break;
    case 0x1C: E = alu_inc(E); break;
    case 0x1D: E = alu_dec(E); break;
    case 0x1E: E = fetch8(); break;
    case 0x1F: {                                                 // RRA
        uint8_t c = (uint8_t)(A & 1);
        A = (uint8_t)((A >> 1) | ((F & FLAG_C) << 7));
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) | c | (A & (FLAG_Y | FLAG_X)));
        break; }

    case 0x20: { int8_t d = (int8_t)fetch8();
        if (!TSTF(FLAG_Z)) { PC = (uint16_t)(PC + d); WZ = PC; z80.cycles += 5; } break; }
    case 0x21: HL = fetch16(); break;
    case 0x22: { uint16_t a = fetch16(); wr16(a, HL); WZ = (uint16_t)(a + 1); break; }
    case 0x23: HL++; break;
    case 0x24: H = alu_inc(H); break;
    case 0x25: H = alu_dec(H); break;
    case 0x26: H = fetch8(); break;
    case 0x27: op_daa(); break;
    case 0x28: { int8_t d = (int8_t)fetch8();
        if (TSTF(FLAG_Z)) { PC = (uint16_t)(PC + d); WZ = PC; z80.cycles += 5; } break; }
    case 0x29: alu_add16(&z80.hl, HL); break;
    case 0x2A: { uint16_t a = fetch16(); HL = rd16(a); WZ = (uint16_t)(a + 1); break; }
    case 0x2B: HL--; break;
    case 0x2C: L = alu_inc(L); break;
    case 0x2D: L = alu_dec(L); break;
    case 0x2E: L = fetch8(); break;
    case 0x2F: A = (uint8_t)~A;                                   // CPL
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P | FLAG_C)) | FLAG_H | FLAG_N |
                      (A & (FLAG_Y | FLAG_X))); break;

    case 0x30: { int8_t d = (int8_t)fetch8();
        if (!TSTF(FLAG_C)) { PC = (uint16_t)(PC + d); WZ = PC; z80.cycles += 5; } break; }
    case 0x31: SP = fetch16(); break;
    case 0x32: { uint16_t a = fetch16(); wr8(a, A);
        WZ = (uint16_t)((A << 8) | ((a + 1) & 0xFF)); break; }
    case 0x33: SP++; break;
    case 0x34: wr8(HL, alu_inc(rd8(HL))); break;
    case 0x35: wr8(HL, alu_dec(rd8(HL))); break;
    case 0x36: wr8(HL, fetch8()); break;
    case 0x37:                                                    // SCF
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) | FLAG_C | (A & (FLAG_Y | FLAG_X)));
        break;
    case 0x38: { int8_t d = (int8_t)fetch8();
        if (TSTF(FLAG_C)) { PC = (uint16_t)(PC + d); WZ = PC; z80.cycles += 5; } break; }
    case 0x39: alu_add16(&z80.hl, SP); break;
    case 0x3A: { uint16_t a = fetch16(); A = rd8(a); WZ = (uint16_t)(a + 1); break; }
    case 0x3B: SP--; break;
    case 0x3C: A = alu_inc(A); break;
    case 0x3D: A = alu_dec(A); break;
    case 0x3E: A = fetch8(); break;
    case 0x3F: {                                                  // CCF
        uint8_t c = (uint8_t)(F & FLAG_C);
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_P)) | (c ? FLAG_H : 0) |
                      (c ^ FLAG_C) | (A & (FLAG_Y | FLAG_X)));
        break; }

    // ---- 0x40..0x7F : LD r,r' -------------------------------------------
    case 0x40: break;             case 0x41: B = C; break;
    case 0x42: B = D; break;      case 0x43: B = E; break;
    case 0x44: B = H; break;      case 0x45: B = L; break;
    case 0x46: B = rd8(HL); break;case 0x47: B = A; break;
    case 0x48: C = B; break;      case 0x49: break;
    case 0x4A: C = D; break;      case 0x4B: C = E; break;
    case 0x4C: C = H; break;      case 0x4D: C = L; break;
    case 0x4E: C = rd8(HL); break;case 0x4F: C = A; break;
    case 0x50: D = B; break;      case 0x51: D = C; break;
    case 0x52: break;             case 0x53: D = E; break;
    case 0x54: D = H; break;      case 0x55: D = L; break;
    case 0x56: D = rd8(HL); break;case 0x57: D = A; break;
    case 0x58: E = B; break;      case 0x59: E = C; break;
    case 0x5A: E = D; break;      case 0x5B: break;
    case 0x5C: E = H; break;      case 0x5D: E = L; break;
    case 0x5E: E = rd8(HL); break;case 0x5F: E = A; break;
    case 0x60: H = B; break;      case 0x61: H = C; break;
    case 0x62: H = D; break;      case 0x63: H = E; break;
    case 0x64: break;             case 0x65: H = L; break;
    case 0x66: H = rd8(HL); break;case 0x67: H = A; break;
    case 0x68: L = B; break;      case 0x69: L = C; break;
    case 0x6A: L = D; break;      case 0x6B: L = E; break;
    case 0x6C: L = H; break;      case 0x6D: break;
    case 0x6E: L = rd8(HL); break;case 0x6F: L = A; break;
    case 0x70: wr8(HL, B); break; case 0x71: wr8(HL, C); break;
    case 0x72: wr8(HL, D); break; case 0x73: wr8(HL, E); break;
    case 0x74: wr8(HL, H); break; case 0x75: wr8(HL, L); break;
    case 0x76: z80.halted = true; PC--; break;                    // HALT
    case 0x77: wr8(HL, A); break;
    case 0x78: A = B; break;      case 0x79: A = C; break;
    case 0x7A: A = D; break;      case 0x7B: A = E; break;
    case 0x7C: A = H; break;      case 0x7D: A = L; break;
    case 0x7E: A = rd8(HL); break;case 0x7F: break;

    // ---- 0x80..0xBF : ALU A,r -------------------------------------------
    case 0x80: alu_add(B); break; case 0x81: alu_add(C); break;
    case 0x82: alu_add(D); break; case 0x83: alu_add(E); break;
    case 0x84: alu_add(H); break; case 0x85: alu_add(L); break;
    case 0x86: alu_add(rd8(HL)); break; case 0x87: alu_add(A); break;
    case 0x88: alu_adc(B); break; case 0x89: alu_adc(C); break;
    case 0x8A: alu_adc(D); break; case 0x8B: alu_adc(E); break;
    case 0x8C: alu_adc(H); break; case 0x8D: alu_adc(L); break;
    case 0x8E: alu_adc(rd8(HL)); break; case 0x8F: alu_adc(A); break;
    case 0x90: alu_sub(B); break; case 0x91: alu_sub(C); break;
    case 0x92: alu_sub(D); break; case 0x93: alu_sub(E); break;
    case 0x94: alu_sub(H); break; case 0x95: alu_sub(L); break;
    case 0x96: alu_sub(rd8(HL)); break; case 0x97: alu_sub(A); break;
    case 0x98: alu_sbc(B); break; case 0x99: alu_sbc(C); break;
    case 0x9A: alu_sbc(D); break; case 0x9B: alu_sbc(E); break;
    case 0x9C: alu_sbc(H); break; case 0x9D: alu_sbc(L); break;
    case 0x9E: alu_sbc(rd8(HL)); break; case 0x9F: alu_sbc(A); break;
    case 0xA0: alu_and(B); break; case 0xA1: alu_and(C); break;
    case 0xA2: alu_and(D); break; case 0xA3: alu_and(E); break;
    case 0xA4: alu_and(H); break; case 0xA5: alu_and(L); break;
    case 0xA6: alu_and(rd8(HL)); break; case 0xA7: alu_and(A); break;
    case 0xA8: alu_xor(B); break; case 0xA9: alu_xor(C); break;
    case 0xAA: alu_xor(D); break; case 0xAB: alu_xor(E); break;
    case 0xAC: alu_xor(H); break; case 0xAD: alu_xor(L); break;
    case 0xAE: alu_xor(rd8(HL)); break; case 0xAF: alu_xor(A); break;
    case 0xB0: alu_or(B); break;  case 0xB1: alu_or(C); break;
    case 0xB2: alu_or(D); break;  case 0xB3: alu_or(E); break;
    case 0xB4: alu_or(H); break;  case 0xB5: alu_or(L); break;
    case 0xB6: alu_or(rd8(HL)); break;  case 0xB7: alu_or(A); break;
    case 0xB8: alu_cp(B); break;  case 0xB9: alu_cp(C); break;
    case 0xBA: alu_cp(D); break;  case 0xBB: alu_cp(E); break;
    case 0xBC: alu_cp(H); break;  case 0xBD: alu_cp(L); break;
    case 0xBE: alu_cp(rd8(HL)); break;  case 0xBF: alu_cp(A); break;

    // ---- 0xC0..0xFF -----------------------------------------------------
    case 0xC0: if (!TSTF(FLAG_Z)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xC1: BC = pop16(); break;
    case 0xC2: { uint16_t a = fetch16(); WZ = a; if (!TSTF(FLAG_Z)) PC = a; break; }
    case 0xC3: PC = fetch16(); WZ = PC; break;
    case 0xC4: { uint16_t a = fetch16(); WZ = a;
        if (!TSTF(FLAG_Z)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xC5: push16(BC); break;
    case 0xC6: alu_add(fetch8()); break;
    case 0xC7: push16(PC); PC = 0x00; WZ = PC; break;
    case 0xC8: if (TSTF(FLAG_Z)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xC9: PC = pop16(); WZ = PC; break;
    case 0xCA: { uint16_t a = fetch16(); WZ = a; if (TSTF(FLAG_Z)) PC = a; break; }
    case 0xCB: exec_cb(); break;
    case 0xCC: { uint16_t a = fetch16(); WZ = a;
        if (TSTF(FLAG_Z)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xCD: { uint16_t a = fetch16(); push16(PC); PC = a; WZ = PC; break; }
    case 0xCE: alu_adc(fetch8()); break;
    case 0xCF: push16(PC); PC = 0x08; WZ = PC; break;

    case 0xD0: if (!TSTF(FLAG_C)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xD1: DE = pop16(); break;
    case 0xD2: { uint16_t a = fetch16(); WZ = a; if (!TSTF(FLAG_C)) PC = a; break; }
    case 0xD3: { uint8_t p = fetch8();                            // OUT (n),A
        cv_bus_out((uint16_t)((A << 8) | p), A);
        WZ = (uint16_t)((A << 8) | ((p + 1) & 0xFF)); break; }
    case 0xD4: { uint16_t a = fetch16(); WZ = a;
        if (!TSTF(FLAG_C)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xD5: push16(DE); break;
    case 0xD6: alu_sub(fetch8()); break;
    case 0xD7: push16(PC); PC = 0x10; WZ = PC; break;
    case 0xD8: if (TSTF(FLAG_C)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xD9: {                                                  // EXX
        uint16_t t;
        t = BC; BC = z80.bc2.w; z80.bc2.w = t;
        t = DE; DE = z80.de2.w; z80.de2.w = t;
        t = HL; HL = z80.hl2.w; z80.hl2.w = t;
        break; }
    case 0xDA: { uint16_t a = fetch16(); WZ = a; if (TSTF(FLAG_C)) PC = a; break; }
    case 0xDB: { uint8_t p = fetch8();                            // IN A,(n)
        uint16_t port = (uint16_t)((A << 8) | p);
        A = cv_bus_in(port);
        WZ = (uint16_t)(port + 1); break; }
    case 0xDC: { uint16_t a = fetch16(); WZ = a;
        if (TSTF(FLAG_C)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xDD: exec_ddfd(&z80.ix); break;
    case 0xDE: alu_sbc(fetch8()); break;
    case 0xDF: push16(PC); PC = 0x18; WZ = PC; break;

    case 0xE0: if (!TSTF(FLAG_P)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xE1: HL = pop16(); break;
    case 0xE2: { uint16_t a = fetch16(); WZ = a; if (!TSTF(FLAG_P)) PC = a; break; }
    case 0xE3: { uint16_t t = rd16(SP); wr16(SP, HL); HL = t; WZ = t; break; } // EX (SP),HL
    case 0xE4: { uint16_t a = fetch16(); WZ = a;
        if (!TSTF(FLAG_P)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xE5: push16(HL); break;
    case 0xE6: alu_and(fetch8()); break;
    case 0xE7: push16(PC); PC = 0x20; WZ = PC; break;
    case 0xE8: if (TSTF(FLAG_P)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xE9: PC = HL; break;                                    // JP (HL)
    case 0xEA: { uint16_t a = fetch16(); WZ = a; if (TSTF(FLAG_P)) PC = a; break; }
    case 0xEB: { uint16_t t = DE; DE = HL; HL = t; break; }       // EX DE,HL
    case 0xEC: { uint16_t a = fetch16(); WZ = a;
        if (TSTF(FLAG_P)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xED: exec_ed(); break;
    case 0xEE: alu_xor(fetch8()); break;
    case 0xEF: push16(PC); PC = 0x28; WZ = PC; break;

    case 0xF0: if (!TSTF(FLAG_S)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xF1: AF = pop16(); break;
    case 0xF2: { uint16_t a = fetch16(); WZ = a; if (!TSTF(FLAG_S)) PC = a; break; }
    case 0xF3: z80.iff1 = z80.iff2 = false; break;                // DI
    case 0xF4: { uint16_t a = fetch16(); WZ = a;
        if (!TSTF(FLAG_S)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xF5: push16(AF); break;
    case 0xF6: alu_or(fetch8()); break;
    case 0xF7: push16(PC); PC = 0x30; WZ = PC; break;
    case 0xF8: if (TSTF(FLAG_S)) { PC = pop16(); WZ = PC; z80.cycles += 6; } break;
    case 0xF9: SP = HL; break;
    case 0xFA: { uint16_t a = fetch16(); WZ = a; if (TSTF(FLAG_S)) PC = a; break; }
    case 0xFB: z80.iff1 = z80.iff2 = true; z80.ei_pending = true; break;  // EI
    case 0xFC: { uint16_t a = fetch16(); WZ = a;
        if (TSTF(FLAG_S)) { push16(PC); PC = a; z80.cycles += 7; } break; }
    case 0xFD: exec_ddfd(&z80.iy); break;
    case 0xFE: alu_cp(fetch8()); break;
    case 0xFF: push16(PC); PC = 0x38; WZ = PC; break;
    }

    return z80.cycles;
}

int HOT(z80_run)(int tstates) {
    int total = 0;
    while (total < tstates) total += z80_step();
    return total;
}

// ---------------------------------------------------------------------------
// CB prefix: rotates, shifts, BIT/RES/SET
// ---------------------------------------------------------------------------
static void HOT(exec_cb)(void) {
    uint8_t op = fetch8();
    bump_r();

    int reg = op & 0x07;
    int idx = (op >> 3) & 0x07;
    int grp = op >> 6;

    uint8_t v;
    if (reg == 6) { v = rd8(HL); z80.cycles += (grp == 1) ? 8 : 11; }
    else {
        z80.cycles += 4;
        switch (reg) {
        case 0: v = B; break; case 1: v = C; break;
        case 2: v = D; break; case 3: v = E; break;
        case 4: v = H; break; case 5: v = L; break;
        default: v = A; break;
        }
    }

    if (grp == 0) {
        switch (idx) {
        case 0: v = op_rlc(v); break; case 1: v = op_rrc(v); break;
        case 2: v = op_rl(v);  break; case 3: v = op_rr(v);  break;
        case 4: v = op_sla(v); break; case 5: v = op_sra(v); break;
        case 6: v = op_sll(v); break; default: v = op_srl(v); break;
        }
    } else if (grp == 1) {
        if (reg == 6) op_bit_mem(v, idx, WZ); else op_bit(v, idx);
        return;                                    // BIT never writes back
    } else if (grp == 2) {
        v = (uint8_t)(v & ~(1 << idx));
    } else {
        v = (uint8_t)(v | (1 << idx));
    }

    if (reg == 6) wr8(HL, v);
    else switch (reg) {
        case 0: B = v; break; case 1: C = v; break;
        case 2: D = v; break; case 3: E = v; break;
        case 4: H = v; break; case 5: L = v; break;
        default: A = v; break;
    }
}

// ---------------------------------------------------------------------------
// ED prefix
// ---------------------------------------------------------------------------
static void HOT(exec_ed)(void) {
    uint8_t op = fetch8();
    bump_r();
    z80.cycles += cyc_ed[op];

    switch (op) {
    // IN r,(C) / OUT (C),r
    case 0x40: B = op_in_c(); break;   case 0x48: C = op_in_c(); break;
    case 0x50: D = op_in_c(); break;   case 0x58: E = op_in_c(); break;
    case 0x60: H = op_in_c(); break;   case 0x68: L = op_in_c(); break;
    case 0x70: (void)op_in_c(); break; case 0x78: A = op_in_c(); WZ = (uint16_t)(BC + 1); break;

    case 0x41: cv_bus_out(BC, B); WZ = (uint16_t)(BC + 1); break;
    case 0x49: cv_bus_out(BC, C); WZ = (uint16_t)(BC + 1); break;
    case 0x51: cv_bus_out(BC, D); WZ = (uint16_t)(BC + 1); break;
    case 0x59: cv_bus_out(BC, E); WZ = (uint16_t)(BC + 1); break;
    case 0x61: cv_bus_out(BC, H); WZ = (uint16_t)(BC + 1); break;
    case 0x69: cv_bus_out(BC, L); WZ = (uint16_t)(BC + 1); break;
    case 0x71: cv_bus_out(BC, 0); WZ = (uint16_t)(BC + 1); break;
    case 0x79: cv_bus_out(BC, A); WZ = (uint16_t)(BC + 1); break;

    // SBC/ADC HL,rr
    case 0x42: alu_sbc16(BC); break;   case 0x4A: alu_adc16(BC); break;
    case 0x52: alu_sbc16(DE); break;   case 0x5A: alu_adc16(DE); break;
    case 0x62: alu_sbc16(HL); break;   case 0x6A: alu_adc16(HL); break;
    case 0x72: alu_sbc16(SP); break;   case 0x7A: alu_adc16(SP); break;

    // LD (nn),rr / LD rr,(nn)
    case 0x43: { uint16_t a = fetch16(); wr16(a, BC); WZ = (uint16_t)(a + 1); break; }
    case 0x53: { uint16_t a = fetch16(); wr16(a, DE); WZ = (uint16_t)(a + 1); break; }
    case 0x63: { uint16_t a = fetch16(); wr16(a, HL); WZ = (uint16_t)(a + 1); break; }
    case 0x73: { uint16_t a = fetch16(); wr16(a, SP); WZ = (uint16_t)(a + 1); break; }
    case 0x4B: { uint16_t a = fetch16(); BC = rd16(a); WZ = (uint16_t)(a + 1); break; }
    case 0x5B: { uint16_t a = fetch16(); DE = rd16(a); WZ = (uint16_t)(a + 1); break; }
    case 0x6B: { uint16_t a = fetch16(); HL = rd16(a); WZ = (uint16_t)(a + 1); break; }
    case 0x7B: { uint16_t a = fetch16(); SP = rd16(a); WZ = (uint16_t)(a + 1); break; }

    // NEG (all eight encodings)
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C: {
        uint8_t v = A; A = 0; alu_sub(v); break; }

    // RETN / RETI
    case 0x45: case 0x55: case 0x5D: case 0x65: case 0x6D: case 0x75: case 0x7D:
    case 0x4D:
        z80.iff1 = z80.iff2;
        PC = pop16(); WZ = PC;
        break;

    // IM n
    case 0x46: case 0x4E: case 0x66: case 0x6E: z80.im = 0; break;
    case 0x56: case 0x76: z80.im = 1; break;
    case 0x5E: case 0x7E: z80.im = 2; break;

    case 0x47: z80.i = A; break;                       // LD I,A
    case 0x4F: z80.r = (uint8_t)(A & 0x7F); z80.r7 = (uint8_t)(A & 0x80); break; // LD R,A
    case 0x57:                                          // LD A,I
        A = z80.i;
        F = (uint8_t)((F & FLAG_C) | sz53_tbl[A] | (z80.iff2 ? FLAG_P : 0));
        break;
    case 0x5F:                                          // LD A,R
        A = get_r();
        F = (uint8_t)((F & FLAG_C) | sz53_tbl[A] | (z80.iff2 ? FLAG_P : 0));
        break;

    case 0x67: op_rrd(); break;
    case 0x6F: op_rld(); break;

    // ---- Block transfer / search / I-O ----------------------------------
    case 0xA0: case 0xB0: {                             // LDI / LDIR
        uint8_t v = rd8(HL);
        wr8(DE, v);
        HL++; DE++; BC--;
        uint8_t n = (uint8_t)(v + A);
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_C)) |
                      (BC ? FLAG_P : 0) |
                      ((n & 0x02) ? FLAG_Y : 0) | ((n & 0x08) ? FLAG_X : 0));
        if (op == 0xB0 && BC) { PC -= 2; WZ = (uint16_t)(PC + 1); z80.cycles += 5; }
        break; }
    case 0xA8: case 0xB8: {                             // LDD / LDDR
        uint8_t v = rd8(HL);
        wr8(DE, v);
        HL--; DE--; BC--;
        uint8_t n = (uint8_t)(v + A);
        F = (uint8_t)((F & (FLAG_S | FLAG_Z | FLAG_C)) |
                      (BC ? FLAG_P : 0) |
                      ((n & 0x02) ? FLAG_Y : 0) | ((n & 0x08) ? FLAG_X : 0));
        if (op == 0xB8 && BC) { PC -= 2; WZ = (uint16_t)(PC + 1); z80.cycles += 5; }
        break; }
    case 0xA1: case 0xB1: {                             // CPI / CPIR
        uint8_t v = rd8(HL);
        uint8_t r = (uint8_t)(A - v);
        uint8_t h = (uint8_t)(((A ^ v ^ r) & FLAG_H));
        HL++; BC--;
        WZ = (uint16_t)(WZ + 1);
        uint8_t n = (uint8_t)(r - (h ? 1 : 0));
        F = (uint8_t)((F & FLAG_C) | FLAG_N | h |
                      (sz53_tbl[r] & FLAG_S) | (r == 0 ? FLAG_Z : 0) |
                      (BC ? FLAG_P : 0) |
                      ((n & 0x02) ? FLAG_Y : 0) | ((n & 0x08) ? FLAG_X : 0));
        if (op == 0xB1 && BC && r) { PC -= 2; WZ = (uint16_t)(PC + 1); z80.cycles += 5; }
        break; }
    case 0xA9: case 0xB9: {                             // CPD / CPDR
        uint8_t v = rd8(HL);
        uint8_t r = (uint8_t)(A - v);
        uint8_t h = (uint8_t)(((A ^ v ^ r) & FLAG_H));
        HL--; BC--;
        WZ = (uint16_t)(WZ - 1);
        uint8_t n = (uint8_t)(r - (h ? 1 : 0));
        F = (uint8_t)((F & FLAG_C) | FLAG_N | h |
                      (sz53_tbl[r] & FLAG_S) | (r == 0 ? FLAG_Z : 0) |
                      (BC ? FLAG_P : 0) |
                      ((n & 0x02) ? FLAG_Y : 0) | ((n & 0x08) ? FLAG_X : 0));
        if (op == 0xB9 && BC && r) { PC -= 2; WZ = (uint16_t)(PC + 1); z80.cycles += 5; }
        break; }
    case 0xA2: case 0xB2: {                             // INI / INIR
        uint8_t v = cv_bus_in(BC);
        WZ = (uint16_t)(BC + 1);
        wr8(HL, v);
        B--; HL++;
        uint16_t t = (uint16_t)(v + ((C + 1) & 0xFF));
        F = (uint8_t)(sz53_tbl[B] | ((v & 0x80) ? FLAG_N : 0) |
                      ((t > 255) ? (FLAG_H | FLAG_C) : 0) |
                      parity_tbl[(t & 7) ^ B]);
        if (op == 0xB2 && B) { PC -= 2; z80.cycles += 5; }
        break; }
    case 0xAA: case 0xBA: {                             // IND / INDR
        uint8_t v = cv_bus_in(BC);
        WZ = (uint16_t)(BC - 1);
        wr8(HL, v);
        B--; HL--;
        uint16_t t = (uint16_t)(v + ((C - 1) & 0xFF));
        F = (uint8_t)(sz53_tbl[B] | ((v & 0x80) ? FLAG_N : 0) |
                      ((t > 255) ? (FLAG_H | FLAG_C) : 0) |
                      parity_tbl[(t & 7) ^ B]);
        if (op == 0xBA && B) { PC -= 2; z80.cycles += 5; }
        break; }
    case 0xA3: case 0xB3: {                             // OUTI / OTIR
        uint8_t v = rd8(HL);
        B--;
        WZ = (uint16_t)(BC + 1);
        cv_bus_out(BC, v);
        HL++;
        uint16_t t = (uint16_t)(v + L);
        F = (uint8_t)(sz53_tbl[B] | ((v & 0x80) ? FLAG_N : 0) |
                      ((t > 255) ? (FLAG_H | FLAG_C) : 0) |
                      parity_tbl[(t & 7) ^ B]);
        if (op == 0xB3 && B) { PC -= 2; z80.cycles += 5; }
        break; }
    case 0xAB: case 0xBB: {                             // OUTD / OTDR
        uint8_t v = rd8(HL);
        B--;
        WZ = (uint16_t)(BC - 1);
        cv_bus_out(BC, v);
        HL--;
        uint16_t t = (uint16_t)(v + L);
        F = (uint8_t)(sz53_tbl[B] | ((v & 0x80) ? FLAG_N : 0) |
                      ((t > 255) ? (FLAG_H | FLAG_C) : 0) |
                      parity_tbl[(t & 7) ^ B]);
        if (op == 0xBB && B) { PC -= 2; z80.cycles += 5; }
        break; }

    default:
        // Every other ED opcode behaves as two NOPs.
        break;
    }
}

// ---------------------------------------------------------------------------
// DD / FD prefix: HL -> IX/IY, (HL) -> (IX+d)/(IY+d)
// ---------------------------------------------------------------------------
static void HOT(exec_ddfd)(Z80Reg *ir) {
    uint8_t op = fetch8();
    bump_r();

    #define IRW  (ir->w)
    #define IRH  (ir->b.h)
    #define IRL  (ir->b.l)

    // Displacement helper for the (IX+d) forms.
    auto disp = [&]() -> uint16_t {
        int8_t d = (int8_t)fetch8();
        uint16_t a = (uint16_t)(IRW + d);
        WZ = a;
        return a;
    };

    switch (op) {
    case 0x09: alu_add16(ir, BC); z80.cycles += 15; break;
    case 0x19: alu_add16(ir, DE); z80.cycles += 15; break;
    case 0x29: alu_add16(ir, IRW); z80.cycles += 15; break;
    case 0x39: alu_add16(ir, SP); z80.cycles += 15; break;

    case 0x21: IRW = fetch16(); z80.cycles += 14; break;
    case 0x22: { uint16_t a = fetch16(); wr16(a, IRW); WZ = (uint16_t)(a + 1);
                 z80.cycles += 20; break; }
    case 0x23: IRW++; z80.cycles += 10; break;
    case 0x2A: { uint16_t a = fetch16(); IRW = rd16(a); WZ = (uint16_t)(a + 1);
                 z80.cycles += 20; break; }
    case 0x2B: IRW--; z80.cycles += 10; break;

    case 0x24: IRH = alu_inc(IRH); z80.cycles += 8; break;
    case 0x25: IRH = alu_dec(IRH); z80.cycles += 8; break;
    case 0x26: IRH = fetch8(); z80.cycles += 11; break;
    case 0x2C: IRL = alu_inc(IRL); z80.cycles += 8; break;
    case 0x2D: IRL = alu_dec(IRL); z80.cycles += 8; break;
    case 0x2E: IRL = fetch8(); z80.cycles += 11; break;

    case 0x34: { uint16_t a = disp(); wr8(a, alu_inc(rd8(a))); z80.cycles += 23; break; }
    case 0x35: { uint16_t a = disp(); wr8(a, alu_dec(rd8(a))); z80.cycles += 23; break; }
    case 0x36: { uint16_t a = disp(); wr8(a, fetch8()); z80.cycles += 19; break; }

    // LD r,IXh/IXl and LD IXh/IXl,r
    case 0x44: B = IRH; z80.cycles += 8; break;
    case 0x45: B = IRL; z80.cycles += 8; break;
    case 0x4C: C = IRH; z80.cycles += 8; break;
    case 0x4D: C = IRL; z80.cycles += 8; break;
    case 0x54: D = IRH; z80.cycles += 8; break;
    case 0x55: D = IRL; z80.cycles += 8; break;
    case 0x5C: E = IRH; z80.cycles += 8; break;
    case 0x5D: E = IRL; z80.cycles += 8; break;
    case 0x7C: A = IRH; z80.cycles += 8; break;
    case 0x7D: A = IRL; z80.cycles += 8; break;
    case 0x60: IRH = B; z80.cycles += 8; break;
    case 0x61: IRH = C; z80.cycles += 8; break;
    case 0x62: IRH = D; z80.cycles += 8; break;
    case 0x63: IRH = E; z80.cycles += 8; break;
    case 0x64: z80.cycles += 8; break;
    case 0x65: IRH = IRL; z80.cycles += 8; break;
    case 0x67: IRH = A; z80.cycles += 8; break;
    case 0x68: IRL = B; z80.cycles += 8; break;
    case 0x69: IRL = C; z80.cycles += 8; break;
    case 0x6A: IRL = D; z80.cycles += 8; break;
    case 0x6B: IRL = E; z80.cycles += 8; break;
    case 0x6C: IRL = IRH; z80.cycles += 8; break;
    case 0x6D: z80.cycles += 8; break;
    case 0x6F: IRL = A; z80.cycles += 8; break;

    // LD r,(IX+d)
    case 0x46: { uint16_t a = disp(); B = rd8(a); z80.cycles += 19; break; }
    case 0x4E: { uint16_t a = disp(); C = rd8(a); z80.cycles += 19; break; }
    case 0x56: { uint16_t a = disp(); D = rd8(a); z80.cycles += 19; break; }
    case 0x5E: { uint16_t a = disp(); E = rd8(a); z80.cycles += 19; break; }
    case 0x66: { uint16_t a = disp(); H = rd8(a); z80.cycles += 19; break; }
    case 0x6E: { uint16_t a = disp(); L = rd8(a); z80.cycles += 19; break; }
    case 0x7E: { uint16_t a = disp(); A = rd8(a); z80.cycles += 19; break; }

    // LD (IX+d),r
    case 0x70: { uint16_t a = disp(); wr8(a, B); z80.cycles += 19; break; }
    case 0x71: { uint16_t a = disp(); wr8(a, C); z80.cycles += 19; break; }
    case 0x72: { uint16_t a = disp(); wr8(a, D); z80.cycles += 19; break; }
    case 0x73: { uint16_t a = disp(); wr8(a, E); z80.cycles += 19; break; }
    case 0x74: { uint16_t a = disp(); wr8(a, H); z80.cycles += 19; break; }
    case 0x75: { uint16_t a = disp(); wr8(a, L); z80.cycles += 19; break; }
    case 0x77: { uint16_t a = disp(); wr8(a, A); z80.cycles += 19; break; }

    // ALU with IXh/IXl
    case 0x84: alu_add(IRH); z80.cycles += 8; break;
    case 0x85: alu_add(IRL); z80.cycles += 8; break;
    case 0x8C: alu_adc(IRH); z80.cycles += 8; break;
    case 0x8D: alu_adc(IRL); z80.cycles += 8; break;
    case 0x94: alu_sub(IRH); z80.cycles += 8; break;
    case 0x95: alu_sub(IRL); z80.cycles += 8; break;
    case 0x9C: alu_sbc(IRH); z80.cycles += 8; break;
    case 0x9D: alu_sbc(IRL); z80.cycles += 8; break;
    case 0xA4: alu_and(IRH); z80.cycles += 8; break;
    case 0xA5: alu_and(IRL); z80.cycles += 8; break;
    case 0xAC: alu_xor(IRH); z80.cycles += 8; break;
    case 0xAD: alu_xor(IRL); z80.cycles += 8; break;
    case 0xB4: alu_or(IRH);  z80.cycles += 8; break;
    case 0xB5: alu_or(IRL);  z80.cycles += 8; break;
    case 0xBC: alu_cp(IRH);  z80.cycles += 8; break;
    case 0xBD: alu_cp(IRL);  z80.cycles += 8; break;

    // ALU with (IX+d)
    case 0x86: { uint16_t a = disp(); alu_add(rd8(a)); z80.cycles += 19; break; }
    case 0x8E: { uint16_t a = disp(); alu_adc(rd8(a)); z80.cycles += 19; break; }
    case 0x96: { uint16_t a = disp(); alu_sub(rd8(a)); z80.cycles += 19; break; }
    case 0x9E: { uint16_t a = disp(); alu_sbc(rd8(a)); z80.cycles += 19; break; }
    case 0xA6: { uint16_t a = disp(); alu_and(rd8(a)); z80.cycles += 19; break; }
    case 0xAE: { uint16_t a = disp(); alu_xor(rd8(a)); z80.cycles += 19; break; }
    case 0xB6: { uint16_t a = disp(); alu_or (rd8(a)); z80.cycles += 19; break; }
    case 0xBE: { uint16_t a = disp(); alu_cp (rd8(a)); z80.cycles += 19; break; }

    case 0xE1: IRW = pop16(); z80.cycles += 14; break;
    case 0xE3: { uint16_t t = rd16(SP); wr16(SP, IRW); IRW = t; WZ = t;
                 z80.cycles += 23; break; }
    case 0xE5: push16(IRW); z80.cycles += 15; break;
    case 0xE9: PC = IRW; z80.cycles += 8; break;
    case 0xF9: SP = IRW; z80.cycles += 10; break;

    // ---- DDCB / FDCB ----------------------------------------------------
    case 0xCB: {
        int8_t d  = (int8_t)fetch8();
        uint8_t o = fetch8();               // note: R is NOT bumped here
        uint16_t a = (uint16_t)(IRW + d);
        WZ = a;

        int idx = (o >> 3) & 0x07;
        int grp = o >> 6;
        int reg = o & 0x07;
        uint8_t v = rd8(a);

        if (grp == 1) {                     // BIT n,(IX+d) -- 20 T
            op_bit_mem(v, idx, a);
            z80.cycles += 20;
            break;
        }
        if (grp == 0) {
            switch (idx) {
            case 0: v = op_rlc(v); break; case 1: v = op_rrc(v); break;
            case 2: v = op_rl(v);  break; case 3: v = op_rr(v);  break;
            case 4: v = op_sla(v); break; case 5: v = op_sra(v); break;
            case 6: v = op_sll(v); break; default: v = op_srl(v); break;
            }
        } else if (grp == 2) {
            v = (uint8_t)(v & ~(1 << idx));
        } else {
            v = (uint8_t)(v | (1 << idx));
        }
        wr8(a, v);
        // Undocumented: the result is also copied into the named register.
        switch (reg) {
        case 0: B = v; break; case 1: C = v; break;
        case 2: D = v; break; case 3: E = v; break;
        case 4: H = v; break; case 5: L = v; break;
        case 7: A = v; break; default: break;
        }
        z80.cycles += 23;
        break; }

    default:
        // Not an indexed opcode -- the prefix is ignored and the byte executes
        // as a normal instruction. Rewinding PC lets the main table handle it
        // on the next step, which matches the real chip's behaviour of treating
        // a stray DD/FD as a 4 T-state prefix-NOP.
        PC--;
        z80.cycles += 4;
        break;
    }

    #undef IRW
    #undef IRH
    #undef IRL
}
