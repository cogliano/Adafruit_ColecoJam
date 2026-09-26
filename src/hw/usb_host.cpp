// usb_host.cpp -- TinyUSB host bring-up, and the translation from normalised
// controller state into ColecoVision controller state.
//
// Report decoding itself lives in hid_app.cpp (ported from pico-infonesPlus),
// which handles USB HID gamepads -- generic DirectInput pads, DualShock 4,
// DualSense, PlayStation Classic, Genesis Mini, Retro-bit MD Arcade, MantaPad
// -- plus XInput pads (Xbox 360 / One / Series) and USB keyboards. This file
// keeps the Fruit-Jam-specific half: powering the USB-A ports, configuring
// PIO-USB, and mapping io::GamePadState / io::KeyboardState onto cv_pad[].
//
// Controller reference:
//   https://learn.adafruit.com/usb-game-controller-with-snes-like-layout
//
// Gamepad mapping, no keyboard attached:
//   D-pad ............................ joystick directions
//   L / R shoulder ................... left / right side action buttons
//   Select ........................... keypad *
//   Start ............................ keypad #
//   A / B / X / Y .................... keypad 1 / 2 / 3 / 4
//   Select + A / B / X / Y ........... keypad 5 / 6 / 7 / 8
//   Start  + A / B ................... keypad 9 / 0
//
// The combination mappings take priority: holding Select and then pressing A
// yields keypad 5, not * followed by 1. Select or Start alone (no face button
// held) is what produces * or #.
//
// Gamepad mapping, keyboard attached:
//   D-pad ............................ joystick directions
//   B / X / L shoulder ............... left side action button
//   A / R shoulder ................... right side action button
//   Select / Start ................... keypad * / #
//   Y and the chords ................. nothing
//
// A keyboard makes the whole keypad directly reachable, so the pad stops
// standing in for it and becomes a plain ColecoVision controller: a joystick
// and two action buttons, which is all the hardware ever had.
//
// Keyboard mapping (merged into port 1; the keyboard never takes a port):
//   Arrow keys ....................... joystick directions
//   Z / X ............................ left / right side action buttons
//   0-9 (number row or numpad) ....... keypad 0-9
//   Shift+8, numpad * ................ keypad *
//   Shift+3, numpad / ................ keypad #
//   A / S ............................ keypad * / # (Select / Start)
//   Enter ............................ confirm, in the ROM browser
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_host.h"
#include "gamepad.h"
#include "config.h"
#include "../emu/coleco.h"

#include "pico/stdlib.h"
#include "tusb.h"
#include "pio_usb.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// Device-level attach counter. Distinct from the HID interface count: the Fruit
// Jam's USB-A ports hang off a CH334F hub, so the hub itself is the first
// device to enumerate. If dev_seen is 0 the bus is dead; if dev_seen is 1 and
// no HID interface is up, the hub came up but the gamepad behind it did not.
static int dev_seen = 0;

// Total HID reports received, across all devices. Shown on screen so it is
// obvious whether a controller is sending anything at all -- a static hex dump
// means either nothing is arriving or the display is not refreshing, and those
// look identical without a counter.
static volatile uint32_t report_count = 0;
static volatile bool host_init_done = false;

// How far usb_host_init() got. It runs on core 1 and cannot draw to the
// screen, so it publishes a step number that core 0 reads and displays. Any
// of the calls below can block or panic, and panics on core 1 are silent.
static volatile int init_step = 0;

// ---------------------------------------------------------------------------
// Raw report capture, for the SHOW_HID_DEBUG menu footer
// ---------------------------------------------------------------------------
// Slots are handed out in arrival order across all HID interfaces, so they do
// NOT line up with player slots -- a keyboard occupies a capture slot but no
// controller port. That is fine for a hex dump whose whole purpose is showing
// what an unidentified device actually sends.
//
// 24 bytes because the buttons on at least one pad tested live past the first
// 8, and a truncated capture makes them look like they report nothing at all.
struct RawCapture {
    bool     used;
    uint8_t  dev_addr;
    uint8_t  instance;
    uint8_t  raw[24];
    uint8_t  raw_len;
    uint16_t report_len;    // true length, even if longer than raw[]
};
static RawCapture captures[MAX_PADS];

void usb_host_note_report(unsigned char dev_addr, unsigned char instance,
                          const uint8_t *report, unsigned short len) {
    report_count++;

    RawCapture *slot = nullptr;
    for (int i = 0; i < MAX_PADS; i++) {
        if (captures[i].used &&
            captures[i].dev_addr == dev_addr && captures[i].instance == instance) {
            slot = &captures[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAX_PADS; i++) {
            if (!captures[i].used) {
                captures[i].used     = true;
                captures[i].dev_addr = dev_addr;
                captures[i].instance = instance;
                slot = &captures[i];
                break;
            }
        }
    }
    if (!slot) return;

    slot->report_len = len;
    slot->raw_len = (uint8_t)(len < sizeof(slot->raw) ? len : sizeof(slot->raw));
    memcpy(slot->raw, report, slot->raw_len);
}

// Blink the step number before each call, the same trick video_init() uses.
// There is a working display by this point, but usb_host.cpp sits below the UI
// layer and the calls here are blocking, so the LED is still the practical
// channel. Enabled by USB_HOST_STAGE_BLINK in config.h.
static void hstage(int n) {
#if !BOOT_DIAGNOSTICS
    (void)n;
    return;
#endif
#if USB_HOST_STAGE_BLINK
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    for (int i = 0; i < n; i++) {
        gpio_put(PIN_LED, 0); sleep_ms(120);
        gpio_put(PIN_LED, 1); sleep_ms(120);
    }
    sleep_ms(500);
#else
    (void)n;
#endif
}

// PIO_USB_TX_DMA_CH is defined in config.h, next to VIDEO_DRIVER, because
// the right value depends on which video driver is built.

// ---------------------------------------------------------------------------
// TinyUSB device-level callbacks
// ---------------------------------------------------------------------------
// The HID callbacks (tuh_hid_mount_cb and friends) live in hid_app.cpp.
// These two fire for every device, hub included.
void tuh_mount_cb(uint8_t dev_addr)   { (void)dev_addr; dev_seen++; }
void tuh_umount_cb(uint8_t dev_addr)  {
    if (dev_seen) dev_seen--;
    for (int i = 0; i < MAX_PADS; i++) {
        if (captures[i].used && captures[i].dev_addr == dev_addr) {
            captures[i].used = false;
        }
    }
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
    hstage(1);
    init_step = 1;
    gpio_put(PIN_USB_HOST_5V_EN, USB_HOST_5V_ACTIVE_HIGH ? 1 : 0);
    init_step = 2;

    hstage(2);
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = (uint8_t)PIN_USB_HOST_DP;

    // Transmit DMA channel.
    //
    // PIO_USB_DEFAULT_CONFIG hardcodes 0 and PIO-USB claims it itself inside
    // tuh_init(). Which channel is safe depends on the video driver:
    //
    //   VIDEO_DRIVER_HSTX      our driver allocates via dma_claim_unused_channel(),
    //                          so channel 0 is reserved below and handed back
    //                          just before tuh_init() claims it.
    //   VIDEO_DRIVER_PICO_HDMI the library takes channels 0 AND 1 by name, so
    //                          PIO-USB must be moved off them entirely. No
    //                          reservation is needed -- nothing else allocates.
    //
    // Setting this explicitly matters: left at the default under pico_hdmi,
    // PIO-USB would claim channel 0 during tuh_init() and panic against the
    // library's own claim.
    pio_cfg.tx_ch = (uint8_t)PIO_USB_TX_DMA_CH;

    hstage(3);
#if VIDEO_DRIVER == VIDEO_DRIVER_HSTX
    // Reserve the transmit channel so video_init()'s dma_claim_unused_channel()
    // calls skip it; released again just before tuh_init() lets PIO-USB claim
    // it properly.
    //
    // Not done for pico_hdmi: that library takes channels 0 and 1 by name and
    // never allocates, so nothing can steal this one and there is nothing to
    // protect it from. Reserving it there added a claim/unclaim pair that had
    // no purpose -- and the unclaim was where the boot faulted.
    dma_channel_claim(PIO_USB_TX_DMA_CH);
#endif
    init_step = 3;

    hstage(4);
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    init_step = 4;
#endif
}

void usb_host_init(void) {
    memset(captures, 0, sizeof(captures));
#if !ENABLE_USB_HOST
    return;
#else
    usb_host_prepare();          // no-op if core 0 already did it
    init_step = 5;

    // Release the channel reserved in usb_host_prepare() so PIO-USB can claim
    // it. video_init() has already run and taken its own channels, so nothing
    // else will grab this one in between.
    hstage(5);
#if VIDEO_DRIVER == VIDEO_DRIVER_HSTX
    // Hand the reserved channel back so PIO-USB can claim it inside tuh_init().
    if (dma_channel_is_claimed(PIO_USB_TX_DMA_CH))
        dma_channel_unclaim(PIO_USB_TX_DMA_CH);
#endif
    init_step = 6;

    hstage(6);
    tuh_init(BOARD_TUH_RHPORT);
    init_step = 7;
    host_init_done = true;
#endif
}

void usb_host_task(void) {
#if ENABLE_USB_HOST
    tuh_task();

    // Retry any HID report request that failed earlier. Without this a single
    // transient failure silences a controller until it is physically
    // unplugged, because the request for the next report is the only thing
    // keeping reports flowing. See hid_app.cpp, note (f).
    hid_app_retry_pending();

    // Once a second, check that every mounted HID interface still has a
    // transfer outstanding, and re-arm any that does not. A transfer can be
    // lost without anything reporting a failure, which leaves a controller
    // silent while it still shows as connected and correctly assigned.
    static absolute_time_t next_keepalive = { 0 };
    if (time_reached(next_keepalive)) {
        next_keepalive = make_timeout_time_ms(1000);
        hid_app_keepalive();
    }
#endif
}

// Live count of mounted HID interfaces, asked of TinyUSB rather than tracked
// with a counter of our own -- hid_app.cpp owns the HID mount callbacks now,
// and a count that cannot drift is the better diagnostic anyway.
int usb_host_hid_seen(void) {
#if ENABLE_USB_HOST
    int n = 0;
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) {
        if (tuh_mounted(addr)) n += tuh_hid_instance_count(addr);
    }
    return n;
#else
    return 0;
#endif
}

int  usb_host_dev_seen(void)    { return dev_seen; }
bool usb_host_init_done(void)   { return host_init_done; }
int  usb_host_init_step(void)   { return init_step; }

int usb_host_raw_report(int index, uint8_t *dst, int max) {
    if (index < 0 || index >= MAX_PADS || !captures[index].used) return 0;
    int n = captures[index].raw_len;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) dst[i] = captures[index].raw[i];
    return n;
}

uint32_t usb_host_report_count(void) { return report_count; }

int usb_host_report_len(int index) {
    if (index < 0 || index >= MAX_PADS || !captures[index].used) return 0;
    return (int)captures[index].report_len;
}

int usb_host_pad_count(void) {
    int n = 0;
    for (int i = 0; i < 2; i++)
        if (io::getCurrentGamePadState(i).isConnected()) n++;
    return n;
}

bool usb_host_keyboard_connected(void) {
    return io::getCurrentKeyboardState().connected;
}

const char *usb_host_pad_name(int index) {
    if (index < 0 || index >= 2) return nullptr;
    auto &gp = io::getCurrentGamePadState(index);
    return gp.isConnected() ? gp.GamePadShortName : nullptr;
}

// ---------------------------------------------------------------------------
// Normalised buttons -> menu bitmask
// ---------------------------------------------------------------------------
// The menu keeps its own MENU_* enum (usb_host.h) as a stable ABI, so
// menu.cpp and wait_for_button_release() are unaffected by how the pad state
// is produced. The keyboard is folded into port 0 so it can drive the browser.
uint16_t usb_host_raw_buttons(int index) {
    if (index < 0 || index >= 2) return 0;

    using Btn = io::GamePadState::Button;
    const io::KeyboardState &kb = io::getCurrentKeyboardState();
    auto &gp = io::getCurrentGamePadState(index);
    uint32_t b = gp.isConnected() ? gp.buttons : 0;
    if (index == 0) b |= kb.buttons;

    uint16_t m = 0;

    // Enter confirms in the browser, and only there -- it is deliberately kept
    // out of KeyboardState::buttons so that pressing it mid-game does not also
    // fire the left action button.
    if (index == 0 && kb.connected) {
        for (int i = 0; i < 6; i++) {
            if (kb.keycode[i] == HID_KEY_ENTER ||
                kb.keycode[i] == HID_KEY_KEYPAD_ENTER) {
                m |= MENU_A;
                break;
            }
        }
    }
    if (b & Btn::UP)     m |= MENU_UP;
    if (b & Btn::DOWN)   m |= MENU_DOWN;
    if (b & Btn::LEFT)   m |= MENU_LEFT;
    if (b & Btn::RIGHT)  m |= MENU_RIGHT;
    if (b & Btn::A)      m |= MENU_A;
    if (b & Btn::B)      m |= MENU_B;
    if (b & Btn::X)      m |= MENU_X;
    if (b & Btn::Y)      m |= MENU_Y;
    if (b & Btn::L)      m |= MENU_L;
    if (b & Btn::R)      m |= MENU_R;
    if (b & Btn::SELECT) m |= MENU_SELECT;
    if (b & Btn::START)  m |= MENU_START;
    return m;
}

// ---------------------------------------------------------------------------
// Keyboard -> ColecoVision keypad
// ---------------------------------------------------------------------------
// cv_pad[].keypad is a single index, not a bitmask -- the real hardware could
// only report one key at a time -- so this returns the first match found in the
// six-slot HID rollover buffer.
//
// Shift matters: Shift+8 is '*' and Shift+3 is '#', so with Shift held those
// two must NOT also read as digits.
static uint8_t keyboard_keypad(void) {
    const io::KeyboardState &kb = io::getCurrentKeyboardState();
    if (!kb.connected) return CV_KEY_NONE;

    const bool shift = (kb.modifier & (KEYBOARD_MODIFIER_LEFTSHIFT |
                                       KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    for (int i = 0; i < 6; i++) {
        const uint8_t k = kb.keycode[i];
        if (!k) continue;

        switch (k) {
        // '*' and '#' first, so Shift wins over the plain digit below.
        case HID_KEY_8:              if (shift) return CV_KEY_STAR; break;
        case HID_KEY_3:              if (shift) return CV_KEY_HASH; break;
        case HID_KEY_KEYPAD_MULTIPLY: return CV_KEY_STAR;
        case HID_KEY_KEYPAD_DIVIDE:   return CV_KEY_HASH;
        default: break;
        }
        if (shift) continue;         // any other shifted key is not a digit

        // HID_KEY_1..HID_KEY_9 are contiguous; 0 sits after 9, not before 1.
        if (k >= HID_KEY_1 && k <= HID_KEY_9)
            return (uint8_t)(CV_KEY_1 + (k - HID_KEY_1));
        if (k == HID_KEY_0)
            return CV_KEY_0;
        if (k >= HID_KEY_KEYPAD_1 && k <= HID_KEY_KEYPAD_9)
            return (uint8_t)(CV_KEY_1 + (k - HID_KEY_KEYPAD_1));
        if (k == HID_KEY_KEYPAD_0)
            return CV_KEY_0;
    }
    return CV_KEY_NONE;
}

// ---------------------------------------------------------------------------
// Translate pad state into ColecoVision controller state
// ---------------------------------------------------------------------------
static void map_pad(uint32_t b, bool kb_present, CVController *out) {
    using Btn = io::GamePadState::Button;

    out->joy = 0;
    out->keypad = CV_KEY_NONE;

    if (b & Btn::UP)    out->joy |= CV_JOY_UP;
    if (b & Btn::DOWN)  out->joy |= CV_JOY_DOWN;
    if (b & Btn::LEFT)  out->joy |= CV_JOY_LEFT;
    if (b & Btn::RIGHT) out->joy |= CV_JOY_RIGHT;

    if (b & Btn::L) out->joy |= CV_BTN_LEFT;
    if (b & Btn::R) out->joy |= CV_BTN_RIGHT;

    const bool sel   = (b & Btn::SELECT) != 0;
    const bool start = (b & Btn::START)  != 0;

    if (kb_present) {
        // The keyboard owns the keypad, so the face buttons become the two
        // action buttons the real controller had, doubling the shoulders --
        // by position. A is the right-hand button on the Nintendo layout, so
        // it is the right action button and B the left. Xbox and PlayStation
        // pads follow, because hid_app.cpp maps them by position as well:
        // Circle and Xbox B report as A, Cross and Xbox A as B.
        if (b & Btn::A) out->joy |= CV_BTN_RIGHT;
        if (b & Btn::B) out->joy |= CV_BTN_LEFT;

        // X doubles as the left action button. The Adafruit NES-style pad
        // runs in SNES mode (MANTAPAD_DEFAULT_MODE in CMakeLists.txt), where
        // its B arrives as X; this keeps that B the left action button, as B
        // is on every other pad. It has no shoulders to fall back on.
        if (b & Btn::X) out->joy |= CV_BTN_LEFT;

        if      (sel)   out->keypad = CV_KEY_STAR;
        else if (start) out->keypad = CV_KEY_HASH;
        return;
    }

    if (sel) {
        if      (b & Btn::A) out->keypad = CV_KEY_5;
        else if (b & Btn::B) out->keypad = CV_KEY_6;
        else if (b & Btn::X) out->keypad = CV_KEY_7;
        else if (b & Btn::Y) out->keypad = CV_KEY_8;
        else                 out->keypad = CV_KEY_STAR;
    } else if (start) {
        if      (b & Btn::A) out->keypad = CV_KEY_9;
        else if (b & Btn::B) out->keypad = CV_KEY_0;
        else                 out->keypad = CV_KEY_HASH;
    } else {
        if      (b & Btn::A) out->keypad = CV_KEY_1;
        else if (b & Btn::B) out->keypad = CV_KEY_2;
        else if (b & Btn::X) out->keypad = CV_KEY_3;
        else if (b & Btn::Y) out->keypad = CV_KEY_4;
    }
}

#if SERIAL_INPUT_LOG
// One console line per change in a port's keypad key or action buttons, taken
// after all mapping -- exactly what the emulated ColecoVision reads. Joystick
// directions are left out on purpose: they change every few frames in play and
// would bury the lines that matter.
static void log_input_changes(void) {
    static CVController last[2] = {{0, CV_KEY_NONE}, {0, CV_KEY_NONE}};
    static const char key_char[] = "0123456789*#";
    const uint8_t act_mask = CV_BTN_LEFT | CV_BTN_RIGHT;

    for (int i = 0; i < 2; i++) {
        const CVController &now = cv_pad[i];
        if (now.keypad == last[i].keypad &&
            (now.joy & act_mask) == (last[i].joy & act_mask)) continue;

        printf("P%d keypad %c  left %s  right %s\n", i + 1,
               now.keypad < 12 ? key_char[now.keypad] : '-',
               (now.joy & CV_BTN_LEFT)  ? "on" : "off",
               (now.joy & CV_BTN_RIGHT) ? "on" : "off");
        last[i] = now;
    }
}
#endif

void usb_host_update_coleco(void) {
    const io::KeyboardState &kb = io::getCurrentKeyboardState();
    const bool kb_present = kb.connected;

    for (int i = 0; i < 2; i++) {
        auto &gp = io::getCurrentGamePadState(i);
        uint32_t b = gp.isConnected() ? gp.buttons : 0;

        // The keyboard never takes a port of its own: it joins port 1, so a
        // gamepad there keeps the joystick and gains two proper fire buttons.
        if (i == 0) b |= kb.buttons;

        map_pad(b, kb_present, &cv_pad[i]);

        // A digit typed on the keyboard outranks the * / # the pad's Select
        // and Start produce -- there is only one keypad register per port.
        if (i == 0 && kb_present) {
            const uint8_t k = keyboard_keypad();
            if (k != CV_KEY_NONE) cv_pad[0].keypad = k;
        }
    }

#if SERIAL_INPUT_LOG
    log_input_changes();
#endif
}
