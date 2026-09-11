// coleco.cpp -- ColecoVision system bus
//
// Memory map
//   0x0000-0x1FFF   BIOS ROM (COLECO.BIN)
//   0x2000-0x5FFF   expansion, reads open bus (0xFF)
//   0x6000-0x7FFF   1 KB RAM, mirrored eight times
//   0x8000-0xFFFF   cartridge ROM
//
// I/O map (only address bits 7..5 are decoded)
//   0x80-0x9F  write  select keypad mode on both controller ports
//   0xA0-0xBF  r/w    VDP: even address = data, odd address = control/status
//   0xC0-0xDF  write  select joystick mode on both controller ports
//   0xE0-0xFF  write  SN76489A
//              read   controller port; address bit 1 picks port 1 or 2
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "coleco.h"
#include "z80.h"
#include "tms9918.h"
#include "sn76489.h"
#include <string.h>

#ifdef PICO_ON_DEVICE
#include "pico.h"
#define HOT __not_in_flash_func
#else
#define HOT(x) x
#endif

CVController cv_pad[2];

static const uint8_t *cv_bios = nullptr;
static const uint8_t *cv_rom  = nullptr;
static uint32_t       cv_rom_len = 0;
static uint8_t        cv_ram[CV_RAM_SIZE];

// Which half of the controller multiplexer is currently selected.
// false = joystick + right button, true = keypad + left button.
static bool keypad_mode = false;

// Previous state of the VDP interrupt line, for edge detection. A file-scope
// Diagnostics. A ColecoVision game's main loop waits on the vertical blank, so
// if these stop advancing while the CPU keeps running, the game is stuck
// waiting for an interrupt that is no longer arriving.
uint32_t cv_nmi_count = 0;

// Exposed for the debug overlay: the latched state of the interrupt line as
// the edge detector last saw it.
bool cv_prev_irq(void) { return vdp.irq; }

// Installed as the VDP's interrupt callback: the ColecoVision wires the VDP
// interrupt output straight to the Z80 /NMI.
static void cv_vdp_irq(void) {
    z80_set_nmi();
    cv_nmi_count++;
    cv_last_nmi_pc  = z80.pc.w;
    cv_reads_at_nmi = vdp_status_reads;
}

// State captured at the moment the last NMI was raised, so the interrupt that
// led to the deadlock can be examined after the fact. Once the machine is
// stuck no further interrupts occur, so the live counters stop telling us
// anything -- these preserve the last one.
uint16_t cv_last_nmi_pc    = 0;   // where the CPU was interrupted
uint32_t cv_reads_at_nmi   = 0;   // status-read count at that instant
uint16_t cv_nmi_vector     = 0;   // what the BIOS NMI hook pointed at

// ---------------------------------------------------------------------------
// The ColecoVision keypad returns a 4-bit code on D0-D3, active low. These are
// the codes the BIOS decoder expects.
// ---------------------------------------------------------------------------
static const uint8_t keypad_code[12] = {
    0x0A,   // 0
    0x0D,   // 1
    0x07,   // 2
    0x0C,   // 3
    0x02,   // 4
    0x03,   // 5
    0x0E,   // 6
    0x05,   // 7
    0x01,   // 8
    0x0B,   // 9
    0x09,   // *
    0x06,   // #
};

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
uint8_t HOT(cv_bus_read)(uint16_t addr) {
    if (addr < 0x2000) {
        return cv_bios ? cv_bios[addr] : 0xFF;
    }
    if (addr < 0x6000) {
        return 0xFF;                            // unmapped expansion area
    }
    if (addr < 0x8000) {
        return cv_ram[addr & (CV_RAM_SIZE - 1)];
    }
    uint32_t off = (uint32_t)(addr - 0x8000);
    if (cv_rom && off < cv_rom_len) return cv_rom[off];
    return 0xFF;
}

void HOT(cv_bus_write)(uint16_t addr, uint8_t value) {
    if (addr >= 0x6000 && addr < 0x8000) {
        cv_ram[addr & (CV_RAM_SIZE - 1)] = value;
    }
    // Everything else is ROM or unmapped; writes are discarded.
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------
static inline uint8_t read_controller(int port) {
    const CVController &p = cv_pad[port];

    if (!keypad_mode) {
        // Joystick half: D0 up, D1 right, D2 down, D3 left, D6 right button.
        // Active low, D4/D5/D7 read high.
        uint8_t v = 0x7F;
        if (p.joy & CV_JOY_UP)    v &= (uint8_t)~0x01;
        if (p.joy & CV_JOY_RIGHT) v &= (uint8_t)~0x02;
        if (p.joy & CV_JOY_DOWN)  v &= (uint8_t)~0x04;
        if (p.joy & CV_JOY_LEFT)  v &= (uint8_t)~0x08;
        if (p.joy & CV_BTN_RIGHT) v &= (uint8_t)~0x40;
        return (uint8_t)(v | 0x30);
    }

    // Keypad half: D0-D3 key code, D6 left button.
    uint8_t v = 0x7F;
    if (p.keypad < 12) v = (uint8_t)((v & 0xF0) | keypad_code[p.keypad]);
    else               v = (uint8_t)((v & 0xF0) | 0x0F);
    if (p.joy & CV_BTN_LEFT) v &= (uint8_t)~0x40;
    return (uint8_t)(v | 0x30);
}

uint8_t HOT(cv_bus_in)(uint16_t port) {
    switch (port & 0xE0) {
    case 0xA0:
        return (port & 0x01) ? vdp_read_status() : vdp_read_data();
    case 0xE0:
        return read_controller((port & 0x02) ? 1 : 0);
    default:
        return 0xFF;
    }
}

void HOT(cv_bus_out)(uint16_t port, uint8_t value) {
    switch (port & 0xE0) {
    case 0x80:  keypad_mode = true;  break;
    case 0xC0:  keypad_mode = false; break;
    case 0xA0:
        if (port & 0x01) vdp_write_control(value);
        else             vdp_write_data(value);
        break;
    case 0xE0:
        psg_write(value);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------
bool cv_init(const uint8_t *bios, uint32_t bios_len) {
    // The VDP interrupt output goes straight to the Z80 /NMI on this machine.
    vdp_set_irq_callback(cv_vdp_irq);
    if (!bios || bios_len < CV_BIOS_SIZE) return false;
    cv_bios = bios;
    z80_init();
    vdp_init();
    psg_init();
    cv_reset();
    return true;
}

void cv_load_rom(const uint8_t *rom, uint32_t len) {
    cv_rom = rom;
    cv_rom_len = (len > CV_ROM_MAX) ? CV_ROM_MAX : len;
}

void cv_reset(void) {
    memset(cv_ram, 0, sizeof(cv_ram));
    memset(cv_pad, 0, sizeof(cv_pad));
    cv_nmi_count = 0;
    cv_pad[0].keypad = CV_KEY_NONE;
    cv_pad[1].keypad = CV_KEY_NONE;
    keypad_mode = false;
    z80_reset();
    vdp_reset();
    psg_reset();
}

void HOT(cv_run_scanline)(uint16_t *line_dest) {
    // Render the line and settle the interrupt FIRST, then run the CPU.
    //
    // The old order ran a whole scanline of CPU time before looking at the VDP,
    // so an interrupt raised at the end of the active display was not delivered
    // until 228 T-states later -- and the handler saw a VDP that had already
    // moved on. Games that read the status register expecting to catch the
    // frame flag could miss it, and anything racing the raster drifted by a
    // line. Checking first bounds the latency to a single instruction.
    vdp_scanline(line_dest);

    // The NMI is raised by the VDP callback installed in cv_init(), at the
    // instant the interrupt line asserts -- not sampled here.
    //
    // This used to be a per-scanline poll of vdp.irq with a remembered
    // previous value. The CPU can read the status register and rewrite
    // register 1 several times within a single scanline, so the line could go
    // asserted -> released -> asserted between two samples. Both samples read
    // "asserted", no edge was seen, no NMI was raised, and with nothing left
    // to read the status register the line stayed asserted permanently. The
    // game then waited forever on a vblank flag its handler never got to set.

    z80_run(CV_CYCLES_PER_LINE);
}

void HOT(cv_run_frame)(uint16_t *fb, int stride) {
    for (int y = 0; y < CV_LINES_PER_FRAME; y++) {
        uint16_t *dest = (vdp.line < VDP_ACTIVE_H && fb)
                       ? (fb + (size_t)vdp.line * stride)
                       : nullptr;
        cv_run_scanline(dest);
    }
}
