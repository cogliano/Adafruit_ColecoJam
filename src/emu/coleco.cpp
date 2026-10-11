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
// Needed for ENABLE_SGM. Without it every #if ENABLE_SGM below reads as 0
// and the module quietly compiles away -- no warning, no error, just a
// machine that ignores the SGM ports.
#include "config.h"
#include "z80.h"
#include "tms9918.h"
#include "sn76489.h"
#include "ay8910.h"
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

#if ENABLE_MEGACART
// MegaCart banking.
//
//   0x8000-0xBFFF   the LAST bank of the ROM, always fixed
//   0xC000-0xFFFF   a switchable 16 KB bank
//
// Note which way round that is. The FIXED half is the lower one, and it holds
// the last bank of the ROM -- which is why a MegaCart carries its AA 55 header
// at the start of its last bank, since that is what the BIOS sees at 0x8000.
//
// The bank is selected by READING from 0xFFC0 + n, not by writing anywhere.
// Those addresses sit inside the switchable window itself, so an ordinary
// instruction fetch there switches banks as a side effect. That is how the
// hardware works and games rely on it.
//
// cv_bank_mask is 0 for a plain cartridge, which switches all of this off.
static uint32_t       cv_bank_mask  = 0;        // banks - 1
static const uint8_t *cv_bank_ptr   = nullptr;  // what 0x8000 currently sees
static const uint8_t *cv_fixed_ptr  = nullptr;  // what 0xC000 always sees
static uint8_t        cv_bank_first = 0;        // bank selected at reset

static inline void cv_set_bank(uint8_t bank) {
    bank &= (uint8_t)cv_bank_mask;
    cv_bank_ptr = cv_rom + (uint32_t)bank * 0x4000u;
}
#endif

// Which half of the controller multiplexer is currently selected.
// false = joystick + right button, true = keypad + left button.
static bool keypad_mode = false;

#if ENABLE_SGM
// Super Game Module memory.
//
// 32 KB covering 0x0000-0x7FFF, indexed by address directly. It supersedes the
// base machine's 1 KB rather than sitting alongside it: with the upper window
// switched on, 0x6000-0x7FFF becomes real contiguous RAM instead of that 1 KB
// mirrored eight times.
//
// Both windows start switched OFF, so a cartridge that knows nothing about the
// module sees exactly the machine it expects.
static uint8_t sgm_ram[0x8000];
static bool    sgm_upper = false;   // 0x2000-0x7FFF, port 0x53 bit 0
static bool    sgm_lower = false;   // 0x0000-0x1FFF over the BIOS, port 0x7F
#endif

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
#if ENABLE_SGM
        if (sgm_lower) return sgm_ram[addr];    // RAM switched in over the BIOS
#endif
        return cv_bios ? cv_bios[addr] : 0xFF;
    }
    if (addr < 0x6000) {
#if ENABLE_SGM
        if (sgm_upper) return sgm_ram[addr];
#endif
        return 0xFF;                            // unmapped expansion area
    }
    if (addr < 0x8000) {
#if ENABLE_SGM
        // With the SGM window open this is contiguous RAM, not the 1 KB
        // mirror -- a game relying on the mirror would see different memory,
        // which is exactly what switching the module in means.
        if (sgm_upper) return sgm_ram[addr];
#endif
        return cv_ram[addr & (CV_RAM_SIZE - 1)];
    }
#if ENABLE_MEGACART
    if (cv_bank_mask) {
        if (addr < 0xC000) return cv_fixed_ptr[addr - 0x8000];

        // Reading the top 64 bytes selects a bank. The byte returned comes
        // from the bank that is now mapped, since the select happens first.
        if (addr >= 0xFFC0) cv_set_bank((uint8_t)(addr & cv_bank_mask));
        return cv_bank_ptr[addr - 0xC000];
    }
#endif
    uint32_t off = (uint32_t)(addr - 0x8000);
    if (cv_rom && off < cv_rom_len) return cv_rom[off];
    return 0xFF;
}

void HOT(cv_bus_write)(uint16_t addr, uint8_t value) {
#if ENABLE_SGM
    if (addr < 0x2000) {
        if (sgm_lower) sgm_ram[addr] = value;
        return;                                 // otherwise it is BIOS ROM
    }
    if (addr < 0x6000) {
        if (sgm_upper) sgm_ram[addr] = value;
        return;
    }
    if (addr < 0x8000) {
        if (sgm_upper) sgm_ram[addr] = value;
        else           cv_ram[addr & (CV_RAM_SIZE - 1)] = value;
        return;
    }
#else
    if (addr >= 0x6000 && addr < 0x8000) {
        cv_ram[addr & (CV_RAM_SIZE - 1)] = value;
    }
#endif
    // Everything else is ROM or unmapped; writes are discarded.
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------
static inline uint8_t read_controller(int port) {
    const CVController &p = cv_pad[port];

    if (!keypad_mode) {
        // Joystick half: D0 up, D1 right, D2 down, D3 left, D6 left button.
        // Active low, D4/D5/D7 read high.
        uint8_t v = 0x7F;
        if (p.joy & CV_JOY_UP)    v &= (uint8_t)~0x01;
        if (p.joy & CV_JOY_RIGHT) v &= (uint8_t)~0x02;
        if (p.joy & CV_JOY_DOWN)  v &= (uint8_t)~0x04;
        if (p.joy & CV_JOY_LEFT)  v &= (uint8_t)~0x08;
        if (p.joy & CV_BTN_LEFT)  v &= (uint8_t)~0x40;
        return (uint8_t)(v | 0x30);
    }

    // Keypad half: D0-D3 key code, D6 right button.
    //
    // The two action buttons used to be the other way round, which mirrored
    // every left/right assignment in play. On the real controller the left
    // button shares D6 with the joystick and the right one with the keypad;
    // ares (cv/controller/gamepad) and SMS Plus (coleco_pio_r) agree.
    uint8_t v = 0x7F;
    if (p.keypad < 12) v = (uint8_t)((v & 0xF0) | keypad_code[p.keypad]);
    else               v = (uint8_t)((v & 0xF0) | 0x0F);
    if (p.joy & CV_BTN_RIGHT) v &= (uint8_t)~0x40;
    return (uint8_t)(v | 0x30);
}

uint8_t HOT(cv_bus_in)(uint16_t port) {
#if ENABLE_SGM
    if ((port & 0xFF) == 0x52) return ay_read_data();
#endif

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
#if ENABLE_SGM
    // The SGM's registers need a finer decode than the 0xE0 grouping below:
    // 0x50-0x53 all fall inside the 0x40 group, which the base machine leaves
    // unused, and 0x7F is inside the 0x60 group. Handled first so the coarse
    // switch never sees them.
    switch (port & 0xFF) {
    case 0x50: ay_write_address(value); return;
    case 0x51: ay_write_data(value);    return;
    case 0x53:
        // Bit 0 switches the 24 KB window at 0x2000-0x7FFF in.
        sgm_upper = (value & 0x01) != 0;
        return;
    case 0x7F:
        // Bit 1 CLEAR maps SGM RAM over the BIOS; set restores the BIOS. The
        // sense is inverted compared with port 0x53, which is a detail worth
        // stating because getting it backwards boots to a dead machine.
        sgm_lower = (value & 0x02) == 0;
        return;
    default:
        break;
    }
#endif

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
#if ENABLE_SGM
    ay_init();
#endif
    cv_reset();
    return true;
}

void cv_load_rom(const uint8_t *rom, uint32_t len) {
    cv_rom = rom;

#if ENABLE_MEGACART
    cv_bank_mask = 0;

    // A MegaCart is a whole number of 16 KB banks, and that number is a power
    // of two because the bank select is an address mask rather than a counter.
    const uint32_t banks = len / 0x4000u;
    if (len > CV_ROM_MAX && banks >= 2 && (banks & (banks - 1)) == 0) {
        cv_bank_mask  = banks - 1;
        cv_fixed_ptr  = rom + (banks - 1) * 0x4000u;
        cv_rom_len    = len;

        // No need to hunt for the header any more. The last bank is
        // permanently at 0x8000, so whatever header the cartridge has is
        // visible from the start whichever bank is selected.
        //
        // The switchable window starts at bank 0; games set it themselves
        // before relying on it.
        cv_bank_first = 0;
        cv_set_bank(cv_bank_first);
        return;
    }
#endif

    cv_rom_len = (len > CV_ROM_MAX) ? CV_ROM_MAX : len;
}

void cv_reset(void) {
    memset(cv_ram, 0, sizeof(cv_ram));
    memset(cv_pad, 0, sizeof(cv_pad));
    cv_nmi_count = 0;
    cv_pad[0].keypad = CV_KEY_NONE;
    cv_pad[1].keypad = CV_KEY_NONE;
    keypad_mode = false;
#if ENABLE_MEGACART
    if (cv_bank_mask) cv_set_bank(cv_bank_first);
#endif
#if ENABLE_SGM
    // The module's RAM keeps its contents across a soft reset, as the hardware
    // does, but the mapping goes back to a plain ColecoVision -- otherwise a
    // standard cartridge loaded after an SGM one would boot with RAM sitting
    // over its BIOS.
    sgm_upper = false;
    sgm_lower = false;
    ay_reset();
#endif
    z80_reset();
    vdp_reset();
    psg_reset();
}

// Render the machine's audio: the ColecoVision's own SN76489, plus the SGM's
// AY-3-8910 mixed over it when a game has used it. Callers use this rather
// than psg_render() directly so they do not have to know whether the module is
// switched in.
uint32_t cv_rom_banks(void) {
#if ENABLE_MEGACART
    return cv_bank_mask ? (cv_bank_mask + 1) : 0;
#else
    return 0;
#endif
}

uint32_t cv_rom_boot_bank(void) {
#if ENABLE_MEGACART
    return cv_bank_first;
#else
    return 0;
#endif
}

uint32_t cv_rom_cur_bank(void) {
#if ENABLE_MEGACART
    if (!cv_bank_mask || !cv_rom) return 0;
    return (uint32_t)((cv_bank_ptr - cv_rom) / 0x4000u);
#else
    return 0;
#endif
}

void cv_render_audio(int16_t *buf, int count) {
    psg_render(buf, count);
#if ENABLE_SGM
    ay_mix(buf, count);
#endif
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
