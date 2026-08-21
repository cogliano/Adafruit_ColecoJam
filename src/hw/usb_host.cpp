// usb_host.cpp -- TinyUSB host: SNES-layout USB gamepads on the Fruit Jam's
// two USB-A ports (via the CH334F hub on the PIO-USB root port).
//
// Controller reference:
//   https://learn.adafruit.com/usb-game-controller-with-snes-like-layout
//
// Button mapping to ColecoVision, per the project spec:
//   D-pad ............................ joystick directions
//   L / R shoulder ................... left / right side action buttons
//   Select ........................... keypad *
//   Start ............................ keypad #
//   A / B / X / Y .................... keypad 1 / 2 / 3 / 4
//   Select + A / B / X / Y ........... keypad 5 / 6 / 7 / 8
//   Start  + A / B ................... keypad 9 / 0
//
// Note that the combination mappings take priority: holding Select and then
// pressing A yields keypad 5, not * followed by 1. Select or Start alone (no
// face button held) is what produces * or #.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_host.h"
#include "config.h"
#include "../emu/coleco.h"

#include "pico/stdlib.h"
#include "tusb.h"
#include "pio_usb.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Generic HID gamepad state
// ---------------------------------------------------------------------------
struct PadState {
    bool     connected;
    uint8_t  dev_addr;
    uint8_t  instance;
    uint16_t buttons;       // normalised bitmask, see PAD_* below

    // Neutral baseline, captured from the first report after connection. The
    // pad is assumed to be untouched at that moment, which is a safe bet since
    // it happens milliseconds after enumeration.
    //
    // Needed because "centred" is not universal: idle axes are 0x80 on some
    // pads and 0x00 on others, and the low nibble that holds the hat on some
    // controllers is used for something else entirely on others -- where a
    // resting value of 0 decodes as a permanently held UP.
    bool     have_baseline;
    uint8_t  base_x, base_y, base_hat;

    // Last raw report, for on-screen diagnosis. 24 bytes because the buttons
    // on at least one pad tested live past the first 8, and a truncated
    // capture makes them look like they report nothing at all.
    uint8_t  raw[24];
    uint8_t  raw_len;
    uint16_t report_len;    // true length, even if longer than raw[]
};

enum {
    PAD_UP     = 1 << 0,
    PAD_DOWN   = 1 << 1,
    PAD_LEFT   = 1 << 2,
    PAD_RIGHT  = 1 << 3,
    PAD_A      = 1 << 4,
    PAD_B      = 1 << 5,
    PAD_X      = 1 << 6,
    PAD_Y      = 1 << 7,
    PAD_L      = 1 << 8,
    PAD_R      = 1 << 9,
    PAD_SELECT = 1 << 10,
    PAD_START  = 1 << 11,
};

static PadState pads[MAX_PADS];

// Counts every HID interface that has ever mounted, gamepad or not. Lets the
// menu tell "nothing is enumerating at all" apart from "a device enumerated
// but was not accepted as a gamepad" -- completely different problems.
static int hid_seen = 0;

// Device-level attach counter. Distinct from hid_seen: the Fruit Jam's USB-A
// ports hang off a CH334F hub, so the hub itself is the first device to
// enumerate. If dev_seen is 0 the bus is dead; if dev_seen is 1 and hid_seen
// is 0, the hub came up but the gamepad behind it did not.
static int dev_seen  = 0;

// Total HID reports received, across all pads. Shown on screen so it is
// obvious whether a controller is sending anything at all -- a static hex dump
// means either nothing is arriving or the display is not refreshing, and those
// look identical without a counter.
static volatile uint32_t report_count = 0;
static volatile bool host_init_done = false;

// How far usb_host_init() got. It runs on core 1 and cannot draw to the
// screen, so it publishes a step number that core 0 reads and displays. Any
// of the calls below can block or panic, and panics on core 1 are silent.
static volatile int init_step = 0;

// PIO_USB_DEFAULT_CONFIG's default transmit DMA channel. Reserved early so the
// video driver's allocator calls skip it, then released just before tuh_init()
// so PIO-USB can claim it in the normal way.
#define PIO_USB_TX_DMA_CH 0

// ---------------------------------------------------------------------------
// Report decoding
//
// The Adafruit SNES-like controller enumerates as a generic HID gamepad with
// an 8-byte report: two analog axes (which the D-pad drives to the extremes),
// a hat, and a button bitfield. Rather than parse the report descriptor we
// decode the common DirectInput-style layout, which covers this controller and
// most of the cheap clones people already own.
// ---------------------------------------------------------------------------
// Byte offsets in the report, measured from a real Adafruit SNES-layout pad:
//
//   0  X axis      0x7F centre, 0x00 left,  0xFF right
//   1  Y axis      0x7F centre, 0x00 up,    0xFF down
//   2  Z axis      unused here
//   3  Rz axis     unused here
//   4  unused      rests at 0x80
//   5  low nibble  hat switch, 0x0F = centred
//      high nibble buttons 1-4: X 0x10, A 0x20, B 0x40, Y 0x80
//   6  L 0x01, R 0x02, Select 0x10, Start 0x20
//   7  unused
//
// This is the standard DirectInput arrangement. An earlier version read the
// hat/button byte at offset 4 rather than 5, so no face or shoulder button was
// ever seen -- the d-pad worked because it comes from the axes.
enum {
    RPT_X        = 0,
    RPT_Y        = 1,
    RPT_HAT_BTN  = 5,
    RPT_BTN2     = 6,
    RPT_MIN_LEN  = 7,
};

enum {                          // byte 5, high nibble
    BIT_X = 0x10, BIT_A = 0x20, BIT_B = 0x40, BIT_Y = 0x80,
};
enum {                          // byte 6
    BIT_L = 0x01, BIT_R = 0x02, BIT_SELECT = 0x10, BIT_START = 0x20,
};

static uint16_t decode_report(PadState *p, const uint8_t *report, uint16_t len) {
    if (len < RPT_MIN_LEN) return 0;

    const uint8_t x       = report[RPT_X];
    const uint8_t y       = report[RPT_Y];
    const uint8_t hat_btn = report[RPT_HAT_BTN];
    const uint8_t btn2    = report[RPT_BTN2];

    // First report after connection defines neutral, so a pad that idles its
    // axes somewhere other than centre still reads correctly.
    if (!p->have_baseline) {
        p->base_x   = x;
        p->base_y   = y;
        p->base_hat = (uint8_t)(hat_btn & 0x0F);
        p->have_baseline = true;
    }

    uint16_t b = 0;

    // D-pad, as a deflection from this pad's own neutral. A digital pad slams
    // the axis to an extreme, so a quarter of full scale is ample.
    const int dx = (int)x - (int)p->base_x;
    const int dy = (int)y - (int)p->base_y;
    if (dx < -64) b |= PAD_LEFT;
    if (dx >  64) b |= PAD_RIGHT;
    if (dy < -64) b |= PAD_UP;
    if (dy >  64) b |= PAD_DOWN;

    // Hat, if this pad drives one. Anything matching the resting value counts
    // as centred, whatever that value is.
    const uint8_t hat = (uint8_t)(hat_btn & 0x0F);
    if (hat != p->base_hat) {
        switch (hat) {
        case 0: b |= PAD_UP; break;
        case 1: b |= PAD_UP | PAD_RIGHT; break;
        case 2: b |= PAD_RIGHT; break;
        case 3: b |= PAD_DOWN | PAD_RIGHT; break;
        case 4: b |= PAD_DOWN; break;
        case 5: b |= PAD_DOWN | PAD_LEFT; break;
        case 6: b |= PAD_LEFT; break;
        case 7: b |= PAD_UP | PAD_LEFT; break;
        default: break;                      // 8 or 0x0F = centred
        }
    }

    if (hat_btn & BIT_X) b |= PAD_X;
    if (hat_btn & BIT_A) b |= PAD_A;
    if (hat_btn & BIT_B) b |= PAD_B;
    if (hat_btn & BIT_Y) b |= PAD_Y;

    if (btn2 & BIT_L)      b |= PAD_L;
    if (btn2 & BIT_R)      b |= PAD_R;
    if (btn2 & BIT_SELECT) b |= PAD_SELECT;
    if (btn2 & BIT_START)  b |= PAD_START;

    return b;
}

// ---------------------------------------------------------------------------
// TinyUSB HID callbacks
// ---------------------------------------------------------------------------
// Fires for every device, hub included.
void tuh_mount_cb(uint8_t dev_addr)   { (void)dev_addr; dev_seen++; }
void tuh_umount_cb(uint8_t dev_addr)  { (void)dev_addr; if (dev_seen) dev_seen--; }

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    hid_seen++;

    uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
    if (proto != HID_ITF_PROTOCOL_NONE) {
        // Boot keyboard/mouse -- not a gamepad, but keep receiving so the
        // device does not stall.
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].connected) {
            pads[i].connected     = true;
            pads[i].dev_addr      = dev_addr;
            pads[i].instance      = instance;
            pads[i].buttons       = 0;
            pads[i].have_baseline = false;   // recapture neutral on reconnect
            pads[i].raw_len       = 0;
            break;
        }
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i].connected &&
            pads[i].dev_addr == dev_addr && pads[i].instance == instance) {
            pads[i].connected = false;
            pads[i].buttons = 0;
        }
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i].connected &&
            pads[i].dev_addr == dev_addr && pads[i].instance == instance) {
            report_count++;
            pads[i].report_len = len;
            pads[i].raw_len = (uint8_t)(len < sizeof(pads[i].raw)
                                        ? len : sizeof(pads[i].raw));
            memcpy(pads[i].raw, report, pads[i].raw_len);
            pads[i].buttons = decode_report(&pads[i], report, len);
            break;
        }
    }
    tuh_hid_receive_report(dev_addr, instance);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
// Called on core 0 BEFORE any TinyUSB initialisation, including tud_init().
// tuh_configure() only records the configuration; whichever code path later
// brings the host up then finds the right pins already stored. Doing this
// after the device stack is already running -- which is what we did before --
// leaves PIO-USB with defaults at the moment it matters.
void usb_host_prepare(void) {
#if ENABLE_USB_HOST
    static bool prepared = false;
    if (prepared) return;
    prepared = true;

    // VBUS is already on: main() enables it immediately after the clocks, so
    // the hub has had time to settle long before we get here. Re-assert it
    // anyway, harmlessly, in case this is ever called without that.
    init_step = 1;
    gpio_put(PIN_USB_HOST_5V_EN, USB_HOST_5V_ACTIVE_HIGH ? 1 : 0);
    init_step = 2;

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = (uint8_t)PIN_USB_HOST_DP;

    // Leave .tx_ch at its default of 0, exactly as the working tester does.
    //
    // Overriding it with a channel from dma_claim_unused_channel() was wrong:
    // PIO-USB claims tx_ch itself during tuh_init(), and hw_claim panics on an
    // already-claimed resource. On core 1 that panic is completely silent, so
    // it looked like tuh_init() hanging -- the same trap as the pio_sm_claim()
    // calls I added and removed earlier.
    //
    // But we cannot simply leave it alone either: video_init() runs after this
    // and takes channels 0 and 1 from the allocator, so PIO-USB's channel 0
    // would end up shared with the DVI scanout.
    //
    // So reserve channel 0 here, before video_init() runs, and release it in
    // usb_host_init() immediately before tuh_init(). Video is pushed to 1 and
    // 2, and PIO-USB gets a clear channel 0 to claim for itself.
    dma_channel_claim(PIO_USB_TX_DMA_CH);
    init_step = 3;

    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    init_step = 4;
#endif
}

void usb_host_init(void) {
    memset(pads, 0, sizeof(pads));
#if !ENABLE_USB_HOST
    return;
#else
    usb_host_prepare();          // no-op if core 0 already did it
    init_step = 5;

    // Release the channel reserved in usb_host_prepare() so PIO-USB can claim
    // it. video_init() has already run and taken its own channels, so nothing
    // else will grab this one in between.
    if (dma_channel_is_claimed(PIO_USB_TX_DMA_CH))
        dma_channel_unclaim(PIO_USB_TX_DMA_CH);
    init_step = 6;

    tuh_init(BOARD_TUH_RHPORT);
    init_step = 7;
    host_init_done = true;
#endif
}

void usb_host_task(void) {
#if ENABLE_USB_HOST
    tuh_task();
#endif
}

int  usb_host_hid_seen(void)    { return hid_seen; }
int  usb_host_dev_seen(void)    { return dev_seen; }
bool usb_host_init_done(void)   { return host_init_done; }
int  usb_host_init_step(void)   { return init_step; }

int usb_host_raw_report(int index, uint8_t *dst, int max) {
    if (index < 0 || index >= MAX_PADS || !pads[index].connected) return 0;
    int n = pads[index].raw_len;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) dst[i] = pads[index].raw[i];
    return n;
}

uint32_t usb_host_report_count(void) { return report_count; }

int usb_host_report_len(int index) {
    if (index < 0 || index >= MAX_PADS || !pads[index].connected) return 0;
    return (int)pads[index].report_len;
}

int usb_host_pad_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PADS; i++) if (pads[i].connected) n++;
    return n;
}

uint16_t usb_host_raw_buttons(int index) {
    if (index < 0 || index >= MAX_PADS) return 0;
    return pads[index].connected ? pads[index].buttons : 0;
}

// ---------------------------------------------------------------------------
// Translate pad state into ColecoVision controller state
// ---------------------------------------------------------------------------
static void map_pad(uint16_t b, CVController *out) {
    out->joy = 0;
    out->keypad = CV_KEY_NONE;

    if (b & PAD_UP)    out->joy |= CV_JOY_UP;
    if (b & PAD_DOWN)  out->joy |= CV_JOY_DOWN;
    if (b & PAD_LEFT)  out->joy |= CV_JOY_LEFT;
    if (b & PAD_RIGHT) out->joy |= CV_JOY_RIGHT;

    if (b & PAD_L) out->joy |= CV_BTN_LEFT;
    if (b & PAD_R) out->joy |= CV_BTN_RIGHT;

    const bool sel   = (b & PAD_SELECT) != 0;
    const bool start = (b & PAD_START)  != 0;

    if (sel) {
        if      (b & PAD_A) out->keypad = CV_KEY_5;
        else if (b & PAD_B) out->keypad = CV_KEY_6;
        else if (b & PAD_X) out->keypad = CV_KEY_7;
        else if (b & PAD_Y) out->keypad = CV_KEY_8;
        else                out->keypad = CV_KEY_STAR;
    } else if (start) {
        if      (b & PAD_A) out->keypad = CV_KEY_9;
        else if (b & PAD_B) out->keypad = CV_KEY_0;
        else                out->keypad = CV_KEY_HASH;
    } else {
        if      (b & PAD_A) out->keypad = CV_KEY_1;
        else if (b & PAD_B) out->keypad = CV_KEY_2;
        else if (b & PAD_X) out->keypad = CV_KEY_3;
        else if (b & PAD_Y) out->keypad = CV_KEY_4;
    }
}

void usb_host_update_coleco(void) {
    for (int i = 0; i < 2; i++) {
        if (pads[i].connected) {
            map_pad(pads[i].buttons, &cv_pad[i]);
        } else {
            cv_pad[i].joy = 0;
            cv_pad[i].keypad = CV_KEY_NONE;
        }
    }
}
