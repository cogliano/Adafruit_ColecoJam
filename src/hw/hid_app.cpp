/*
 * author : Shuichi TAKANO
 * since  : Thu Jul 29 2021 03:39:11
 */

// Ported from pico-infonesPlus (pico_shared/hid_app.cpp). Kept as close to
// upstream as possible so its fixes stay easy to merge; every deviation is
// marked with a "ColecoJam:" comment. There are five:
//
//   (a) A keyboard does not claim a player slot. Upstream gives it one, which
//       would cost us a ColecoVision controller port; here it is latched
//       separately (like the mouse) and merged into port 1 by usb_host.cpp.
//       Its state is also cleared on unmount -- upstream leaves keys stuck.
//   (b) Keyboard reports fill io::KeyboardState rather than a GamePadState,
//       which follows from (a). Enter confirms in the ROM browser, but that
//       is handled in usb_host.cpp, not in the key table here.
//   (c) The generic HID path keeps ColecoJam's own DirectInput decoder for
//       reports of 7 bytes or more. Upstream's HID_USAGE_DESKTOP_JOYSTICK
//       case assumes a 4-byte {axis[3], buttons} layout and its
//       HID_USAGE_DESKTOP_GAMEPAD case is an empty stub; the Adafruit
//       SNES-layout pad this project targets puts its hat and buttons at
//       offsets 5 and 6 of an 8-byte report, so upstream's code alone would
//       leave it dead.
//   (d) The parsed report-descriptor cache is keyed on (dev_addr, instance).
//       Upstream keys it on instance alone, but instance is numbered per
//       device -- a keyboard and a gamepad both enumerate as instance 0 and
//       overwrite each other's descriptor. That is exactly the case this port
//       exists to support.
//   (e) Raw reports are copied out for the SHOW_HID_DEBUG menu footer.
//
// ABSWAPPED is left at upstream's default of 1. Despite the name, that does
// not swap A and B on every pad: it selects the Nintendo arrangement by
// position, where the right face button is A and the bottom one is B.
// Nintendo-labelled pads -- the Adafruit NES- and SNES-style controllers, which
// report as a MantaPad (081f:e401), and the DirectInput clones -- keep their
// printed labels; Xbox and PlayStation pads are remapped by position. Setting
// it to 0 swaps A and B on the Adafruit pads, so their A no longer starts a
// game from the ROM browser.
//
// MANTAPAD_DEFAULT_MODE is set to 2 (SNES) by CMakeLists.txt; see there.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <tusb.h>
#include <stdio.h>
#include <string.h>
#include "xinput_host.h"
#include "gamepad.h"
#include "usb_host.h"
#ifndef ABSWAPPED
#define ABSWAPPED 1
#endif

// set to 1 to enable printing of button states in binary when they change
#ifndef PRINTFBUTTONS
#define PRINTFBUTTONS 0
#endif

// Default mode for the MantaPad (081f:e401), which is sold both as a NES-
// and as a SNES-shaped controller with the same VID/PID: 1 = NES behavior
// (SNES mode activated by pressing Y), 2 = SNES from the start. SNES
// emulators should define this as 2 so the pad works fully at connect.
#ifndef MANTAPAD_DEFAULT_MODE
#define MANTAPAD_DEFAULT_MODE 1
#endif

int abSwapped = ABSWAPPED;
int isManta[2] = {0, 0};        // 1 NES, 2 SNES (per player)
int isMantaVariant[2] = {0, 0}; // SNES pad (per player)
#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_REPORT 4

    namespace
    {
        // ColecoJam deviation (d): keyed on (dev_addr, instance).
        //
        // Upstream indexes _report_count[instance] / _report_info_arr[instance]
        // by instance alone. TinyUSB numbers instances per device, so a
        // keyboard on device 2 and a gamepad on device 3 are both instance 0
        // and clobber each other's parsed descriptor -- whichever enumerated
        // last wins, and the other device's reports are then decoded against
        // the wrong layout. Keyboard + gamepad together is the whole point of
        // this port, so the cache is keyed properly here.
        struct ReportCache
        {
            uint8_t dev_addr; // 0 = free slot
            uint8_t instance;
            uint8_t count;
            tuh_hid_report_info_t info[MAX_REPORT];
        };
        static constexpr int MAX_REPORT_CACHE = 8;
        static ReportCache _report_cache[MAX_REPORT_CACHE];

        static ReportCache *findReportCache(uint8_t dev_addr, uint8_t instance)
        {
            for (auto &c : _report_cache)
                if (c.dev_addr == dev_addr && c.instance == instance)
                    return &c;
            return nullptr;
        }

        // Returns the existing entry for this interface, or claims a free one.
        // Null only if more than MAX_REPORT_CACHE HID interfaces are live at
        // once, which CFG_TUH_HID already bounds below that.
        static ReportCache *acquireReportCache(uint8_t dev_addr, uint8_t instance)
        {
            if (auto *c = findReportCache(dev_addr, instance))
                return c;
            for (auto &c : _report_cache)
            {
                if (c.dev_addr == 0)
                {
                    c.dev_addr = dev_addr;
                    c.instance = instance;
                    c.count = 0;
                    return &c;
                }
            }
            return nullptr;
        }

        static void releaseReportCache(uint8_t dev_addr, uint8_t instance)
        {
            if (auto *c = findReportCache(dev_addr, instance))
                c->dev_addr = 0;
        }

        static constexpr int MAX_USB_PLAYERS = 2;
        static uint8_t playerDevAddr[MAX_USB_PLAYERS] = {0, 0};

        // Boot-protocol mouse tracking. A mouse does not occupy a player
        // slot, so a keyboard + mouse + gamepad can all be connected at once.
        static bool mouseMounted = false;
        static uint8_t mouseDevAddr = 0;
        static uint8_t mouseInstance = 0;

        // ColecoJam deviation (a): keyboard tracking, same shape as the mouse
        // above. A keyboard claims no player slot -- usb_host.cpp merges it
        // into controller port 1 instead, so a gamepad stays player 1 and the
        // keyboard supplies the 12-key ColecoVision keypad.
        static bool kbMounted = false;
        static uint8_t kbDevAddr = 0;
        static uint8_t kbInstance = 0;

        static void clearKeyboardState()
        {
            auto &kb = io::keyboardState();
            kb.connected = false;
            kb.modifier = 0;
            kb.buttons = 0;
            memset(kb.keycode, 0, sizeof(kb.keycode));
        }

        // ColecoJam deviation (b). Upstream writes the keyboard's joystick-
        // style mapping into a borrowed GamePadState; it lands in
        // KeyboardState::buttons here. The key table is otherwise unchanged.
        // With the default abSwapped, Z reports B and X reports A, which
        // usb_host.cpp turns into the left and right action buttons -- the
        // order the keys sit in.
        //
        // Enter is deliberately NOT in this table even though it confirms in
        // the ROM browser: usb_host.cpp adds it there only, so pressing Enter
        // mid-game does not also fire the left action button.
        static void processKeyboardReport(hid_keyboard_report_t const *r)
        {
            auto &kb = io::keyboardState();
            kb.connected = true;
            kb.modifier = r->modifier;
            memcpy(kb.keycode, r->keycode, sizeof(kb.keycode));
            kb.buttons = 0;
            for (uint8_t i = 0; i < 6; i++)
            {
                switch (r->keycode[i])
                {
                case HID_KEY_A:
                    kb.buttons |= io::GamePadState::Button::SELECT;
                    break;
                case HID_KEY_S:
                    kb.buttons |= io::GamePadState::Button::START;
                    break;
                case HID_KEY_Z:
                    kb.buttons |= (abSwapped ? io::GamePadState::Button::B : io::GamePadState::Button::A);
                    break;
                case HID_KEY_X:
                    kb.buttons |= (abSwapped ? io::GamePadState::Button::A : io::GamePadState::Button::B);
                    break;
                case HID_KEY_C:
                    kb.buttons |= io::GamePadState::Button::X;
                    break;
                case HID_KEY_V:
                    kb.buttons |= io::GamePadState::Button::Y;
                    break;
                case HID_KEY_Q:
                    kb.buttons |= io::GamePadState::Button::L;
                    break;
                case HID_KEY_W:
                    kb.buttons |= io::GamePadState::Button::R;
                    break;
                case HID_KEY_ARROW_UP:
                    kb.buttons |= io::GamePadState::Button::UP;
                    break;
                case HID_KEY_ARROW_DOWN:
                    kb.buttons |= io::GamePadState::Button::DOWN;
                    break;
                case HID_KEY_ARROW_LEFT:
                    kb.buttons |= io::GamePadState::Button::LEFT;
                    break;
                case HID_KEY_ARROW_RIGHT:
                    kb.buttons |= io::GamePadState::Button::RIGHT;
                    break;
                default:
                    break;
                }
            }
        }

        // ColecoJam deviation (c): the decoder usb_host.cpp used before this
        // port, moved here so the generic HID path keeps working.
        //
        // The Adafruit SNES-layout pad enumerates as a plain HID gamepad with
        // an 8-byte DirectInput report -- two analog axes the d-pad drives to
        // the extremes, a hat, and a button bitfield:
        //
        //   0  X axis      0x7F centre, 0x00 left, 0xFF right
        //   1  Y axis      0x7F centre, 0x00 up,   0xFF down
        //   5  low nibble  hat switch, 0x0F = centred
        //      high nibble buttons 1-4: X 0x10, A 0x20, B 0x40, Y 0x80
        //   6  L 0x01, R 0x02, Select 0x10, Start 0x20
        //
        // Reading the hat/button byte at offset 4 rather than 5 was an earlier
        // bug in this decoder: the d-pad worked (it comes from the axes) while
        // no face or shoulder button was ever seen.
        enum
        {
            RPT_X = 0,
            RPT_Y = 1,
            RPT_HAT_BTN = 5,
            RPT_BTN2 = 6,
            RPT_MIN_LEN = 7,
        };
        enum
        { // byte 5, high nibble
            BIT_X = 0x10,
            BIT_A = 0x20,
            BIT_B = 0x40,
            BIT_Y = 0x80,
        };
        enum
        { // byte 6
            BIT_L = 0x01,
            BIT_R = 0x02,
            BIT_SELECT = 0x10,
            BIT_START = 0x20,
        };

        // Neutral baseline, captured from the first report after connection.
        // "Centred" is not universal: idle axes are 0x80 on some pads and 0x00
        // on others, and the low nibble that holds the hat on one controller is
        // used for something else on another -- where a resting value of 0
        // decodes as a permanently held UP.
        struct DirectInputBaseline
        {
            bool have{false};
            uint8_t x, y, hat;
        };
        static DirectInputBaseline diBaseline[MAX_USB_PLAYERS];

        static void processDirectInputReport(int player, const uint8_t *report, uint16_t len)
        {
            if (len < RPT_MIN_LEN)
                return;

            using Btn = io::GamePadState::Button;

            const uint8_t x = report[RPT_X];
            const uint8_t y = report[RPT_Y];
            const uint8_t hat_btn = report[RPT_HAT_BTN];
            const uint8_t btn2 = report[RPT_BTN2];

            auto &base = diBaseline[player];
            if (!base.have)
            {
                base.x = x;
                base.y = y;
                base.hat = (uint8_t)(hat_btn & 0x0F);
                base.have = true;
            }

            uint32_t b = 0;

            // D-pad as a deflection from this pad's own neutral. A digital pad
            // slams the axis to an extreme, so a quarter of full scale is ample.
            const int dx = (int)x - (int)base.x;
            const int dy = (int)y - (int)base.y;
            if (dx < -64)
                b |= Btn::LEFT;
            if (dx > 64)
                b |= Btn::RIGHT;
            if (dy < -64)
                b |= Btn::UP;
            if (dy > 64)
                b |= Btn::DOWN;

            // Hat, if this pad drives one. Anything matching the resting value
            // counts as centred, whatever that value is.
            const uint8_t hat = (uint8_t)(hat_btn & 0x0F);
            if (hat != base.hat)
            {
                switch (hat)
                {
                case 0: b |= Btn::UP; break;
                case 1: b |= Btn::UP | Btn::RIGHT; break;
                case 2: b |= Btn::RIGHT; break;
                case 3: b |= Btn::DOWN | Btn::RIGHT; break;
                case 4: b |= Btn::DOWN; break;
                case 5: b |= Btn::DOWN | Btn::LEFT; break;
                case 6: b |= Btn::LEFT; break;
                case 7: b |= Btn::UP | Btn::LEFT; break;
                default: break; // 8 or 0x0F = centred
                }
            }

            if (hat_btn & BIT_X)
                b |= Btn::X;
            // Same convention as the MantaPad decoder in
            // tuh_hid_report_received_cb(): these are Nintendo-labelled pads,
            // so the default abSwapped keeps A as A.
            if (hat_btn & BIT_A)
                b |= abSwapped ? Btn::A : Btn::B;
            if (hat_btn & BIT_B)
                b |= abSwapped ? Btn::B : Btn::A;
            if (hat_btn & BIT_Y)
                b |= Btn::Y;

            if (btn2 & BIT_L)
                b |= Btn::L;
            if (btn2 & BIT_R)
                b |= Btn::R;
            if (btn2 & BIT_SELECT)
                b |= Btn::SELECT;
            if (btn2 & BIT_START)
                b |= Btn::START;

            auto &gp = io::getCurrentGamePadState(player);
            gp.buttons = b;
            gp.axis[0] = x;
            gp.axis[1] = y;
            gp.flagConnected(true);
        }

        static void processMouseReport(hid_mouse_report_t const *r, uint16_t len)
        {
            auto &m = io::getCurrentMouseState();
            m.connected = true;
            m.buttons = r->buttons;
            m.dx += r->x;
            m.dy += r->y;
            if (len >= 4) // wheel-less mice send 3-byte reports
            {
                m.wheel += r->wheel;
            }
        }

        static int assignPlayer(uint8_t dev_addr)
        {
            for (int i = 0; i < MAX_USB_PLAYERS; i++)
                if (playerDevAddr[i] == dev_addr)
                    return i;
            for (int i = 0; i < MAX_USB_PLAYERS; i++)
            {
                if (playerDevAddr[i] == 0)
                {
                    playerDevAddr[i] = dev_addr;
                    return i;
                }
            }
            return -1;
        }

        static void unassignPlayer(uint8_t dev_addr)
        {
            for (int i = 0; i < MAX_USB_PLAYERS; i++)
                if (playerDevAddr[i] == dev_addr)
                {
                    playerDevAddr[i] = 0;
                    return;
                }
        }

        static int getPlayerIndex(uint8_t dev_addr)
        {
            for (int i = 0; i < MAX_USB_PLAYERS; i++)
                if (playerDevAddr[i] == dev_addr)
                    return i;
            return -1;
        }

        // Is dual shock 4 controller detected?
        static inline bool isDS4(uint16_t vid, uint16_t pid)
        {
            return vid == 0x054c && (pid == 0x09cc || pid == 0x05c4);
        }
        // Is dual sense controller detected?
        static inline bool isDS5(uint16_t vid, uint16_t pid)
        {
            return vid == 0x054c && pid == 0x0ce6;
        }
        // Is PSClassic controller detected?
        static inline bool isPSClassic(uint16_t vid, uint16_t pid)
        {
            return vid == 0x054c && pid == 0x0cda;
        }
        // Is Nintendo controller detected?
        static inline bool isNintendo(uint16_t vid, uint16_t pid)
        {
            return vid == 0x057e && (pid == 0x2009 || pid == 0x2017);
        }
        static inline bool isGenesisMini(uint16_t vid, uint16_t pid)
        {
            return vid == 0x0ca3 && (pid == 0x0025 || pid == 0x0024);
        }
        // Retro-bit 8 button Mega Drive Arcade Pad with USB
        static inline bool isMDArcadePad(uint16_t vid, uint16_t pid)
        {
            return (vid == 0xf0d) && (pid == 0x00c1);
        }
        // Is MantaPad detected? Cheap Aliexpress SNES/NES controller
        static inline bool isMantaPad(uint16_t vid, uint16_t pid)
        {
            return vid == 0x081f && pid == 0xe401;
        }
        // Is MantaPad Variant detected? Cheap Aliexpress SNES controller (0810:e501)
        static inline bool isMantaPadVariant(uint16_t vid, uint16_t pid)
        {
            return vid == 0x0810 && pid == 0xe501;
        }

        struct DS4Report
        {
            // https://www.psdevwiki.com/ps4/DS4-USB

            struct Button1
            {
                inline static constexpr int SQUARE = 1 << 4;
                inline static constexpr int CROSS = 1 << 5;
                inline static constexpr int CIRCLE = 1 << 6;
                inline static constexpr int TRIANGLE = 1 << 7;
            };

            struct Button2
            {
                inline static constexpr int L1 = 1 << 0;
                inline static constexpr int R1 = 1 << 1;
                inline static constexpr int L2 = 1 << 2;
                inline static constexpr int R2 = 1 << 3;
                inline static constexpr int SHARE = 1 << 4;
                inline static constexpr int OPTIONS = 1 << 5;
                inline static constexpr int L3 = 1 << 6;
                inline static constexpr int R3 = 1 << 7;
            };

            uint8_t reportID;
            uint8_t stickL[2];
            uint8_t stickR[2];
            uint8_t buttons1;
            uint8_t buttons2;
            uint8_t ps : 1;
            uint8_t tpad : 1;
            uint8_t counter : 6;
            uint8_t triggerL;
            uint8_t triggerR;
            // ...

            int getHat() const { return buttons1 & 15; }
        };

        struct DS5Report
        {
            uint8_t reportID;
            uint8_t stickL[2];
            uint8_t stickR[2];
            uint8_t triggerL;
            uint8_t triggerR;
            uint8_t counter;
            uint8_t buttons[3];
            // ...

            struct Button
            {
                inline static constexpr int SQUARE = 1 << 4;
                inline static constexpr int CROSS = 1 << 5;
                inline static constexpr int CIRCLE = 1 << 6;
                inline static constexpr int TRIANGLE = 1 << 7;
                inline static constexpr int L1 = 1 << 8;
                inline static constexpr int R1 = 1 << 9;
                inline static constexpr int L2 = 1 << 10;
                inline static constexpr int R2 = 1 << 11;
                inline static constexpr int SHARE = 1 << 12;
                inline static constexpr int OPTIONS = 1 << 13;
                inline static constexpr int L3 = 1 << 14;
                inline static constexpr int R3 = 1 << 15;
                inline static constexpr int PS = 1 << 16;
                inline static constexpr int TPAD = 1 << 17;
            };

            int getHat() const { return buttons[0] & 15; }
        };

        // Report for Genesis Mini controller and Retro-bit 8 button Mega Drive Arcade Pad with USB
        struct GenesisMiniReport
        {
            uint8_t byte1;
            uint8_t byte2;
            uint8_t byte3;
            uint8_t byte4;
            uint8_t byte5;
            uint8_t byte6;
            uint8_t byte7;
            uint8_t byte8;
            struct Button
            {

                inline static constexpr int A = 0b01000000; // byte 6
                inline static constexpr int B = 0b00100000; // byte 6
                inline static constexpr int C = 0b00000010; // byte 7
                inline static constexpr int X = 0b10001111; // byte 6
                inline static constexpr int Y = 0b00011111; // byte 6
                inline static constexpr int Z = 0b00000001; // byte 6
                inline static constexpr int MODE = 0b00010000;   // byte 7
                inline static constexpr int START = 0b00100000;  // byte 7
                inline static constexpr int UP = 0;              // byte 5
                inline static constexpr int DOWN = 0b11111111;   // byte 5
                inline static constexpr int LEFT = 0;            // byte 4
                inline static constexpr int RIGHT = 0b11111111;  // byte 4
                ;
            };
            struct ButtonRetrobit
            {

                inline static constexpr int A = 0b00000100;  // byte 1
                inline static constexpr int B = 0b00000010;  // byte 1
                inline static constexpr int C = 0b10000000;  // byte 1
                inline static constexpr int X = 0b00001000;  // byte 1
                inline static constexpr int Y = 0b00000001;  // byte 1
                inline static constexpr int Z = 0b01000000;  // byte 1
                inline static constexpr int MODE = 0b00000001;     // byte 2
                inline static constexpr int L = 0b00010000;  // byte 1
                inline static constexpr int R = 0b00100000;  // byte 1
                inline static constexpr int START = 0b00000010; // byte 2
                inline static constexpr int UP = 0;             // byte 5
                inline static constexpr int DOWN = 0b11111111;  // byte 5
                inline static constexpr int LEFT = 0;           // byte 4
                inline static constexpr int RIGHT = 0b11111111; // byte 4
                ;
            };
        };

        // Report for MantaPad, cheap AliExpress SNES controller
        struct MantaPadReport
        {
            uint8_t byte1;
            uint8_t byte2;
            uint8_t byte3;
            uint8_t byte4;
            uint8_t byte5;
            uint8_t byte6;
            uint8_t byte7;
            uint8_t byte8;

            struct Button
            {
                inline static constexpr int A = 0b00100000;
                inline static constexpr int B = 0b01000000;
                inline static constexpr int X = 0b00010000;
                inline static constexpr int Y = 0b10000000;
                inline static constexpr int NESB = X; // B Button on NES controller is X on SNES controller
                inline static constexpr int SELECT = 0b00010000;
                inline static constexpr int START = 0b00100000;
                inline static constexpr int UP = 0b00000000;
                inline static constexpr int DOWN = 0b11111111;
                inline static constexpr int LEFT = 0b00000000;
                inline static constexpr int RIGHT = 0b11111111;
                inline static constexpr int SHOULDERLEFT = 0b00000001;
                inline static constexpr int SHOULDERRIGHT = 0b00000010;
            };
        };
        // Report for PSClassic controller
        struct PSClassicReport
        {
            uint8_t buttons;
            // Idle      00010100
            // up        00000100
            // upright   00001000
            // right     00011000
            // rightdown 00101000
            // down      00100100
            // downleft  00100000
            // left      00010000
            // leftup    00100000
            // start     00010110
            // select    00010101
            // St + sel  00010111
            // selectup  00000101
            // selectdown 00100101
            uint8_t hat;
            struct Button
            {
                inline static constexpr int ButtonsIdle = 0x00;
                inline static constexpr int HatIdle = 0b00010100;
                inline static constexpr int Square = 0x08;
                inline static constexpr int Circle = 0x02;
                inline static constexpr int Cross = 0x04;
                inline static constexpr int Triangle = 0x01;
                inline static constexpr int SELECT = 0b00010101;
                inline static constexpr int START = 0b00010110;
                inline static constexpr int UP = 0b00000100;
                inline static constexpr int UPRIGHT = 0b00001000;
                inline static constexpr int RIGHT = 0b00011000;
                inline static constexpr int RIGHTDOWN = 0b00101000;
                inline static constexpr int DOWN = 0b00100100;
                inline static constexpr int DOWNLEFT = 0b00100000;
                inline static constexpr int LEFT = 0b00010000;
                inline static constexpr int LEFTUP = 0b00000000;
                inline static constexpr int SELECTUP = 0b00000101;
                inline static constexpr int SELECTDOWN = 0b00100101;
                inline static constexpr int SELECTSTART = 0b00010111;
            };
        };

        // Helper: print a HID report (any length up to MAX) as binary only when it changes.
        static void printHIDReportIfChanged(const uint8_t *report, uint16_t len)
        {
            if (!report || !len)
                return;
            // Typical HID report sizes are <= 64 bytes for full-speed devices
            constexpr uint16_t MAX_PRINT = 64; // adjust if you expect bigger
            static uint8_t prev_report[MAX_PRINT];
            static uint16_t prev_len = 0;
            static bool prev_valid = false;

            if (len > MAX_PRINT)
            {
                // Fallback: if size exceeds buffer, just print length notice once (or always?)
                // Print raw truncated binary when it changes relative to first MAX_PRINT bytes.
                // Compare only first MAX_PRINT bytes.
                bool changed_large = !prev_valid || prev_len != len || memcmp(prev_report, report, MAX_PRINT) != 0;
                if (!changed_large)
                    return;
                printf("HID report (len=%u > MAX_PRINT=%u) (changed bytes in []): ", len, MAX_PRINT);
                for (uint16_t i = 0; i < MAX_PRINT; ++i)
                {
                    bool byte_changed = !prev_valid || prev_report[i] != report[i];
                    if (byte_changed)
                        putchar('[');
                    uint8_t v = report[i];
                    for (int b = 7; b >= 0; --b)
                        putchar((v & (1 << b)) ? '1' : '0');
                    if (byte_changed)
                        putchar(']');
                    if (i + 1 < MAX_PRINT)
                        putchar(' ');
                }
                putchar('\n');
                memcpy(prev_report, report, MAX_PRINT);
                prev_len = len; // store true len for change detection
                prev_valid = true;
                return;
            }

            bool changed = !prev_valid || prev_len != len || memcmp(prev_report, report, len) != 0;
            if (!changed)
                return;

            printf("HID report (len=%u) (bin, changed bytes in []): ", len);
            for (uint16_t i = 0; i < len; ++i)
            {
                bool byte_changed = !prev_valid || prev_report[i] != report[i];
                if (byte_changed)
                    putchar('[');
                uint8_t v = report[i];
                for (int b = 7; b >= 0; --b)
                    putchar((v & (1 << b)) ? '1' : '0');
                if (byte_changed)
                    putchar(']');
                if (i + 1 < len)
                    putchar(' ');
            }
            putchar('\n');
            memcpy(prev_report, report, len);
            prev_len = len;
            prev_valid = true;
        }
    }

    void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
    {
        uint16_t vid, pid;
        tuh_vid_pid_get(dev_addr, &vid, &pid);

        // ColecoJam deviation (a): keyboards are handled separately and do not
        // claim a player slot either. Detected here from the boot interface
        // protocol; a keyboard that exposes no boot interface is caught later
        // from its report descriptor, in tuh_hid_report_received_cb().
        if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD)
        {
            printf("Keyboard detected - device address = %d, instance = %d is mounted - VID = %04x, PID = %04x\n",
                   dev_addr, instance, vid, pid);
            kbMounted = true;
            kbDevAddr = dev_addr;
            kbInstance = instance;
            io::keyboardState().connected = true;
            if (!tuh_hid_receive_report(dev_addr, instance))
            {
                printf("Error: cannot request to receive report\r\n");
            }
            return;
        }

        // Mice are handled separately and do not claim a player slot.
        if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_MOUSE)
        {
            printf("Mouse detected - device address = %d, instance = %d is mounted - VID = %04x, PID = %04x\n",
                   dev_addr, instance, vid, pid);
            mouseMounted = true;
            mouseDevAddr = dev_addr;
            mouseInstance = instance;
            io::getCurrentMouseState().connected = true;
            if (!tuh_hid_receive_report(dev_addr, instance))
            {
                printf("Error: cannot request to receive report\r\n");
            }
            return;
        }
        int player = assignPlayer(dev_addr);
        if (player < 0)
        {
            printf("No free player slot for HID device address = %d (VID = %04x, PID = %04x)\n", dev_addr, vid, pid);
            return;
        }
        isManta[player] = 0;
        isMantaVariant[player] = 0;
        diBaseline[player].have = false; // recapture neutral on reconnect
        auto &gp = io::getCurrentGamePadState(player);

        if (isDS4(vid, pid))
        {
            printf("Dual Shock 4 Controller detected - device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            gp.GamePadName = "Dual Shock 4";
            gp.GamePadShortName = "DS4";
        }
        else if (isDS5(vid, pid))
        {
            printf("Dual Sense Controller detected - device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            gp.GamePadName = "Dual Sense";
            gp.GamePadShortName = "DS5";
        }
        else if (isMantaPad(vid, pid))
        {
            printf("MantaPad detected - device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            isManta[player] = MANTAPAD_DEFAULT_MODE;
            if (isManta[player] == 2)
            {
                gp.GamePadName = "Manta SNES";
                gp.GamePadShortName = "MSNES";
            }
            else
            {
                printf("Press Y to activate SNES mode\n");
                gp.GamePadName = "Manta NES";
                gp.GamePadShortName = "MNES";
            }
        }
        else if (isMantaPadVariant(vid, pid))
        {
            printf("MantaPad Variant detected - device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            isManta[player] = 2;
            isMantaVariant[player] = 1;
            gp.GamePadName = "Manta SNES";
            gp.GamePadShortName = "MSNES";
        }
        else if (isGenesisMini(vid, pid))
        {
            printf("Sega Mega Drive/Genesis Mini %d controller detected - device address = %d, instance = %d, player %d is mounted - ", (pid == 0x0025) ? 1 : 2, dev_addr, instance, player + 1);
            gp.GamePadName = (pid == 0x0025) ? "Genesis Mini 1" : "Genesis Mini 2";
            gp.GamePadShortName = (pid == 0x0025) ? "GM1" : "GM2";
        }
        else if (isMDArcadePad(vid, pid))
        {
            printf("Retro-bit MD Arcade pad detected - device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            gp.GamePadName = "MDArcade";
            gp.GamePadShortName = "MD";
        }
        else if (isPSClassic(vid, pid))
        {
            printf("PlayStation Classic controller detected - device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            gp.GamePadName = "PSClassic";
            gp.GamePadShortName = "PSC";
        }
        else
        {
            printf("Unkown device detected - HID device address = %d, instance = %d, player %d is mounted - ", dev_addr, instance, player + 1);
            static char unknownName[20];
            snprintf(unknownName, sizeof(unknownName), "%04x:%04x", vid, pid);
            gp.GamePadName = unknownName;
            gp.GamePadShortName = "??";
        }
        printf("VID = %04x, PID = %04x\r\n", vid, pid);
        const char *protocol_str[] = {"None", "Keyboard", "Mouse"}; // hid_protocol_type_t
        uint8_t const interface_protocol = tuh_hid_interface_protocol(dev_addr, instance);

        // Parse report descriptor with built-in parser.
        // ColecoJam deviation (d): cached per (dev_addr, instance).
        if (auto *cache = acquireReportCache(dev_addr, instance))
        {
            cache->count = tuh_hid_parse_report_descriptor(cache->info, MAX_REPORT, desc_report, desc_len);
            printf("HID has %u reports and interface protocol = %d:%s\n", cache->count,
                   interface_protocol, protocol_str[interface_protocol]);
        }
        else
        {
            printf("No free report-descriptor cache slot for device %d instance %d\n", dev_addr, instance);
        }

        if (!tuh_hid_receive_report(dev_addr, instance))
        {
            printf("Error: cannot request to receive report\r\n");
        }
    }

    void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
    {
        releaseReportCache(dev_addr, instance);

        // ColecoJam deviation (a): clear the keyboard on unplug. Upstream
        // never does, so whatever was held stays latched forever.
        if (kbMounted && dev_addr == kbDevAddr && instance == kbInstance)
        {
            printf("Keyboard device address = %d, instance = %d is unmounted\n", dev_addr, instance);
            kbMounted = false;
            clearKeyboardState();
            return;
        }

        if (mouseMounted && dev_addr == mouseDevAddr && instance == mouseInstance)
        {
            printf("Mouse device address = %d, instance = %d is unmounted\n", dev_addr, instance);
            mouseMounted = false;
            auto &m = io::getCurrentMouseState();
            m.connected = false;
            m.buttons = 0;
            return;
        }
        int player = getPlayerIndex(dev_addr);
        if (player >= 0)
        {
            printf("HID device address = %d, instance = %d, player %d is unmounted\n", dev_addr, instance, player + 1);
            auto &gp = io::getCurrentGamePadState(player);
            gp.flagConnected(false);
            gp.GamePadName = nullptr;
            gp.GamePadShortName = nullptr;
            unassignPlayer(dev_addr);
        }
        else
        {
            printf("HID device address = %d, instance = %d is unmounted (was not assigned)\n", dev_addr, instance);
        }
    }

    void tuh_hid_report_received_cb(uint8_t dev_addr,
                                    uint8_t instance, uint8_t const *report, uint16_t len)
    {
        // ColecoJam deviation (e): hand the raw bytes to usb_host.cpp for the
        // SHOW_HID_DEBUG menu footer, which is how an unrecognised pad's
        // layout gets worked out from real data rather than guessed at.
        usb_host_note_report(dev_addr, instance, report, len);

        // ColecoJam deviation (a): boot-protocol keyboard, latched at mount.
        if (kbMounted && dev_addr == kbDevAddr && instance == kbInstance)
        {
            if (len >= sizeof(hid_keyboard_report_t))
            {
                processKeyboardReport(reinterpret_cast<hid_keyboard_report_t const *>(report));
            }
            if (!tuh_hid_receive_report(dev_addr, instance))
            {
                printf("Error: cannot request to receive report\r\n");
            }
            return;
        }

        if (mouseMounted && dev_addr == mouseDevAddr && instance == mouseInstance)
        {
            // Boot-protocol mouse report (TinyUSB host defaults interfaces to
            // boot protocol): buttons, x, y, wheel.
            if (len >= 3)
            {
                processMouseReport(reinterpret_cast<hid_mouse_report_t const *>(report), len);
            }
            if (!tuh_hid_receive_report(dev_addr, instance))
            {
                printf("Error: cannot request to receive report\r\n");
            }
            return;
        }
        int player = getPlayerIndex(dev_addr);
        if (player < 0)
        {
            tuh_hid_receive_report(dev_addr, instance);
            return;
        }

        // ColecoJam deviation (d): keyed on (dev_addr, instance), not instance.
        ReportCache *cache = findReportCache(dev_addr, instance);
        uint8_t const rpt_count = cache ? cache->count : 0;
        tuh_hid_report_info_t *rpt_info_arr = cache ? cache->info : nullptr;
        tuh_hid_report_info_t *rpt_info = NULL;

        uint16_t vid, pid;
        tuh_vid_pid_get(dev_addr, &vid, &pid);
#if PRINTFBUTTONS
        // helper for figuring out button states
        printHIDReportIfChanged(report, len);
#endif
        if (isDS4(vid, pid))
        {
            if (sizeof(DS4Report) <= len)
            {
                auto r = reinterpret_cast<const DS4Report *>(report);
                if (r->reportID != 1)
                {
                    printf("Invalid reportID %d\n", r->reportID);
                    return;
                }

                auto &gp = io::getCurrentGamePadState(player);
                gp.axis[0] = r->stickL[0];
                gp.axis[1] = r->stickL[1];
                if (abSwapped)
                {
                    gp.buttons = (r->buttons1 & DS4Report::Button1::CROSS ? io::GamePadState::Button::B : 0) |
                                 (r->buttons1 & DS4Report::Button1::CIRCLE ? io::GamePadState::Button::A : 0);
                }
                else
                {
                    gp.buttons = (r->buttons1 & DS4Report::Button1::CROSS ? io::GamePadState::Button::A : 0) |
                                 (r->buttons1 & DS4Report::Button1::CIRCLE ? io::GamePadState::Button::B : 0);
                }

                gp.buttons = gp.buttons |
                             (r->buttons1 & DS4Report::Button1::TRIANGLE ? io::GamePadState::Button::X : 0) |
                             (r->buttons1 & DS4Report::Button1::SQUARE ? io::GamePadState::Button::Y : 0) |
                             (r->buttons2 & (DS4Report::Button2::L1 | DS4Report::Button2::L2) ? io::GamePadState::Button::L : 0) |
                             (r->buttons2 & (DS4Report::Button2::R1 | DS4Report::Button2::R2) ? io::GamePadState::Button::R : 0) |
                             (r->buttons2 & DS4Report::Button2::SHARE ? io::GamePadState::Button::SELECT : 0) |
                             (r->tpad ? io::GamePadState::Button::SELECT : 0) |
                             (r->buttons2 & DS4Report::Button2::OPTIONS ? io::GamePadState::Button::START : 0);
                gp.hat = static_cast<io::GamePadState::Hat>(r->getHat());
                gp.convertButtonsFromAxis(0, 1);
                gp.convertButtonsFromHat();
                gp.flagConnected(true);
            }
            else
            {
                printf("Invalid DS4 report size %zd\n", len);
                return;
            }
        }
        else if (isDS5(vid, pid))
        {
            if (sizeof(DS5Report) <= len)
            {

                auto r = reinterpret_cast<const DS5Report *>(report);
                if (r->reportID != 1)
                {
                    printf("Invalid reportID %d\n", r->reportID);
                    return;
                }

                auto buttons = r->buttons[0] | (r->buttons[1] << 8) | (r->buttons[2] << 16);

                auto &gp = io::getCurrentGamePadState(player);
                gp.axis[0] = r->stickL[0];
                gp.axis[1] = r->stickL[1];
                if (abSwapped)
                {
                    gp.buttons =
                        (buttons & DS5Report::Button::CROSS ? io::GamePadState::Button::B : 0) |
                        (buttons & DS5Report::Button::CIRCLE ? io::GamePadState::Button::A : 0);
                }
                else
                {
                    gp.buttons = (buttons & DS5Report::Button::CIRCLE ? io::GamePadState::Button::B : 0) |
                                 (buttons & DS5Report::Button::CROSS ? io::GamePadState::Button::A : 0);
                }
                gp.buttons =
                    gp.buttons |
                    (buttons & DS5Report::Button::TRIANGLE ? io::GamePadState::Button::X : 0) |
                    (buttons & DS5Report::Button::SQUARE ? io::GamePadState::Button::Y : 0) |
                    (buttons & (DS5Report::Button::L1 | DS5Report::Button::L2) ? io::GamePadState::Button::L : 0) |
                    (buttons & (DS5Report::Button::R1 | DS5Report::Button::R2) ? io::GamePadState::Button::R : 0) |
                    (buttons & (DS5Report::Button::SHARE | DS5Report::Button::TPAD) ? io::GamePadState::Button::SELECT : 0) |
                    (buttons & DS5Report::Button::OPTIONS ? io::GamePadState::Button::START : 0);
                gp.hat = static_cast<io::GamePadState::Hat>(r->getHat());
                gp.convertButtonsFromAxis(0, 1);
                gp.convertButtonsFromHat();
                gp.flagConnected(true);
            }
            else
            {
                printf("Invalid DS5 report size %zd\n", len);
                return;
            }
        }
        else if (isMantaPad(vid, pid) || isMantaPadVariant(vid, pid))
        {
            if (sizeof(MantaPadReport) == len)
            {
                auto r = reinterpret_cast<const MantaPadReport *>(report);
                auto &gp = io::getCurrentGamePadState(player);
                uint8_t udb = r->byte2;
                uint8_t lrb = r->byte1;

                // When using AlieExpress SNES usb controller, activate SNES mode by pressing Y
                if (r->byte6 & MantaPadReport::Button::Y)
                {
                    isManta[player] = 2;
                    gp.GamePadName = "Manta SNES";
                    gp.GamePadShortName = "MSNES";
                    // printf("MantaPad SNES mode activated\n");
                }
                // printf("MantaPad report: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                //        r->byte1, r->byte2, r->byte3, r->byte4, r->byte5, r->byte6, r->byte7, r->byte8);

                if (isMantaVariant[player])
                {
                    udb = r->byte5; // UP / DOWN
                    lrb = r->byte4; // LEFT / RIGHT
                }
                if (abSwapped)
                {
                    gp.buttons =
                        (r->byte6 & MantaPadReport::Button::A ? io::GamePadState::Button::A : 0) |
                        ((isManta[player] == 1 && r->byte6 & MantaPadReport::Button::NESB) ? io::GamePadState::Button::B : 0) |
                        ((isManta[player] == 2 && r->byte6 & MantaPadReport::Button::B) ? io::GamePadState::Button::B : 0) |
                        ((isManta[player] == 2 && r->byte6 & MantaPadReport::Button::X) ? io::GamePadState::Button::X : 0) |
                        ((isManta[player] == 2 && r->byte6 & MantaPadReport::Button::Y) ? io::GamePadState::Button::Y : 0) |
                        (r->byte7 & MantaPadReport::Button::SHOULDERLEFT ? io::GamePadState::Button::L : 0) |
                        (r->byte7 & MantaPadReport::Button::SHOULDERRIGHT ? io::GamePadState::Button::R : 0) |
                        (r->byte7 & MantaPadReport::Button::START ? io::GamePadState::Button::START : 0) |
                        (r->byte7 & MantaPadReport::Button::SELECT ? io::GamePadState::Button::SELECT : 0) |
                        (udb == MantaPadReport::Button::UP ? io::GamePadState::Button::UP : 0) |
                        (udb == MantaPadReport::Button::DOWN ? io::GamePadState::Button::DOWN : 0) |
                        (lrb == MantaPadReport::Button::LEFT ? io::GamePadState::Button::LEFT : 0) |
                        (lrb == MantaPadReport::Button::RIGHT ? io::GamePadState::Button::RIGHT : 0);
                }
                else
                {
                    gp.buttons =
                        (r->byte6 & MantaPadReport::Button::A ? io::GamePadState::Button::B : 0) |
                        ((isManta[player] == 1 && r->byte6 & MantaPadReport::Button::NESB) ? io::GamePadState::Button::A : 0) |
                        ((isManta[player] == 2 && r->byte6 & MantaPadReport::Button::B) ? io::GamePadState::Button::A : 0) |
                        ((isManta[player] == 2 && r->byte6 & MantaPadReport::Button::X) ? io::GamePadState::Button::X : 0) |
                        ((isManta[player] == 2 && r->byte6 & MantaPadReport::Button::Y) ? io::GamePadState::Button::Y : 0) |
                        (r->byte7 & MantaPadReport::Button::SHOULDERLEFT ? io::GamePadState::Button::L : 0) |
                        (r->byte7 & MantaPadReport::Button::SHOULDERRIGHT ? io::GamePadState::Button::R : 0) |
                        (r->byte7 & MantaPadReport::Button::START ? io::GamePadState::Button::START : 0) |
                        (r->byte7 & MantaPadReport::Button::SELECT ? io::GamePadState::Button::SELECT : 0) |
                        (udb == MantaPadReport::Button::UP ? io::GamePadState::Button::UP : 0) |
                        (udb == MantaPadReport::Button::DOWN ? io::GamePadState::Button::DOWN : 0) |
                        (lrb == MantaPadReport::Button::LEFT ? io::GamePadState::Button::LEFT : 0) |
                        (lrb == MantaPadReport::Button::RIGHT ? io::GamePadState::Button::RIGHT : 0);
                }
                gp.flagConnected(true);
            }
            else
            {
                printf("Invalid MantaPad report size %zd\n", len);
                return;
            }
        }
        else if (isGenesisMini(vid, pid))
        {
            if (sizeof(GenesisMiniReport) == len)
            {
                

                auto r = reinterpret_cast<const GenesisMiniReport *>(report);

                auto &gp = io::getCurrentGamePadState(player);
                if (abSwapped)
                {
                    gp.buttons = (r->byte6 & GenesisMiniReport::Button::B ? io::GamePadState::Button::A : 0) |
                                 (r->byte6 & GenesisMiniReport::Button::A ? io::GamePadState::Button::B : 0);
                }
                else
                {
                    gp.buttons = (r->byte6 & GenesisMiniReport::Button::A ? io::GamePadState::Button::A : 0) |
                                 (r->byte6 & GenesisMiniReport::Button::B ? io::GamePadState::Button::B : 0);
                }
                gp.buttons = gp.buttons |
                             (((r->byte7 & GenesisMiniReport::Button::C) && pid == 0x0025)? io::GamePadState::Button::SELECT : 0) |  // C button is SELECT only original 3-button controller
                             ((r->byte7 & GenesisMiniReport::Button::C) ? io::GamePadState::Button::C : 0) |
                             (r->byte7 & GenesisMiniReport::Button::START ? io::GamePadState::Button::START : 0) |
                             (r->byte7 & GenesisMiniReport::Button::MODE ? io::GamePadState::Button::SELECT : 0) |
                             (r->byte5 == GenesisMiniReport::Button::UP ? io::GamePadState::Button::UP : 0) |
                             (r->byte5 == GenesisMiniReport::Button::DOWN ? io::GamePadState::Button::DOWN : 0) |
                             (r->byte4 == GenesisMiniReport::Button::LEFT ? io::GamePadState::Button::LEFT : 0) |
                             (r->byte4 == GenesisMiniReport::Button::RIGHT ? io::GamePadState::Button::RIGHT : 0);
                gp.flagConnected(true);
            }
            else
            {
                printf("Invalid Genesis Mini report size %zd\n", len);
                return;
            }
        }
        else if (isMDArcadePad(vid, pid))
        {
            if (sizeof(GenesisMiniReport) == len)
            {
                auto r = reinterpret_cast<const GenesisMiniReport *>(report);

                auto &gp = io::getCurrentGamePadState(player);
                if (abSwapped)
                {
                    gp.buttons = (r->byte1 & GenesisMiniReport::ButtonRetrobit::B ? io::GamePadState::Button::A : 0) |
                                 (r->byte1 & GenesisMiniReport::ButtonRetrobit::A ? io::GamePadState::Button::B : 0);
                }
                else
                {
                    gp.buttons = (r->byte1 & GenesisMiniReport::ButtonRetrobit::A ? io::GamePadState::Button::A : 0) |
                                 (r->byte1 & GenesisMiniReport::ButtonRetrobit::B ? io::GamePadState::Button::B : 0);
                }
                gp.buttons = gp.buttons |
                             (r->byte2 & GenesisMiniReport::ButtonRetrobit::MODE ? io::GamePadState::Button::SELECT : 0) |
                             (r->byte1 & GenesisMiniReport::ButtonRetrobit::C ? io::GamePadState::Button::C : 0) |
                             (r->byte1 & GenesisMiniReport::ButtonRetrobit::X ? io::GamePadState::Button::X : 0) |
                             (r->byte1 & GenesisMiniReport::ButtonRetrobit::Y ? io::GamePadState::Button::Y : 0) |
                             (r->byte1 & GenesisMiniReport::ButtonRetrobit::Z ? io::GamePadState::Button::Z : 0) |
                             (r->byte1 & GenesisMiniReport::ButtonRetrobit::L ? io::GamePadState::Button::L : 0) |
                             (r->byte1 & GenesisMiniReport::ButtonRetrobit::R ? io::GamePadState::Button::R : 0) |
                             (r->byte2 & GenesisMiniReport::ButtonRetrobit::START ? io::GamePadState::Button::START : 0) |
                             (r->byte5 == GenesisMiniReport::ButtonRetrobit::UP ? io::GamePadState::Button::UP : 0) |
                             (r->byte5 == GenesisMiniReport::ButtonRetrobit::DOWN ? io::GamePadState::Button::DOWN : 0) |
                             (r->byte4 == GenesisMiniReport::ButtonRetrobit::LEFT ? io::GamePadState::Button::LEFT : 0) |
                             (r->byte4 == GenesisMiniReport::ButtonRetrobit::RIGHT ? io::GamePadState::Button::RIGHT : 0);
                gp.flagConnected(true);
            }
            else
            {
                printf("Invalid Genesis Mini report size %zd\n", len);
                return;
            }
        }
        else if (isPSClassic(vid, pid))
        {
            if (sizeof(PSClassicReport) == len)
            {
                auto r = reinterpret_cast<const PSClassicReport *>(report);
                auto &gp = io::getCurrentGamePadState(player);
                if (abSwapped)
                {
                    gp.buttons = (r->buttons & PSClassicReport::Button::Cross ? io::GamePadState::Button::B : 0) |
                                 (r->buttons & PSClassicReport::Button::Circle ? io::GamePadState::Button::A : 0);
                }
                else
                {
                    gp.buttons = (r->buttons & PSClassicReport::Button::Circle ? io::GamePadState::Button::B : 0) |
                                 (r->buttons & PSClassicReport::Button::Cross ? io::GamePadState::Button::A : 0);
                }
                gp.buttons |= (r->buttons & PSClassicReport::Button::Triangle ? io::GamePadState::Button::X : 0) |
                              (r->buttons & PSClassicReport::Button::Square ? io::GamePadState::Button::Y : 0);
                // L1/L2/R1/R2 bit positions are not verified yet; capture a
                // report with PRINTFBUTTONS=1 before mapping them to L/R.
                switch (r->hat)
                {
                case PSClassicReport::Button::UP:
                    gp.buttons = gp.buttons | io::GamePadState::Button::UP;
                    break;
                case PSClassicReport::Button::UPRIGHT:
                    gp.buttons = gp.buttons | io::GamePadState::Button::UP | io::GamePadState::Button::RIGHT;
                    break;
                case PSClassicReport::Button::RIGHT:
                    gp.buttons = gp.buttons | io::GamePadState::Button::RIGHT;
                    break;
                case PSClassicReport::Button::RIGHTDOWN:
                    gp.buttons = gp.buttons | io::GamePadState::Button::RIGHT | io::GamePadState::Button::DOWN;
                    break;
                case PSClassicReport::Button::DOWN:
                    gp.buttons = gp.buttons | io::GamePadState::Button::DOWN;
                    break;
                case PSClassicReport::Button::DOWNLEFT:
                    gp.buttons = gp.buttons | io::GamePadState::Button::DOWN | io::GamePadState::Button::LEFT;
                    break;
                case PSClassicReport::Button::LEFT:
                    gp.buttons = gp.buttons | io::GamePadState::Button::LEFT;
                    break;
                case PSClassicReport::Button::LEFTUP:
                    gp.buttons = gp.buttons | io::GamePadState::Button::LEFT | io::GamePadState::Button::UP;
                    break;
                case PSClassicReport::Button::SELECT:
                    gp.buttons = gp.buttons | io::GamePadState::Button::SELECT;
                    break;
                case PSClassicReport::Button::START:
                    gp.buttons = gp.buttons | io::GamePadState::Button::START;
                    break;
                case PSClassicReport::Button::SELECTSTART:
                    gp.buttons = gp.buttons | io::GamePadState::Button::SELECT | io::GamePadState::Button::START;
                    break;
                case PSClassicReport::Button::SELECTUP:
                    gp.buttons = gp.buttons | io::GamePadState::Button::SELECT | io::GamePadState::Button::UP;
                    break;
                case PSClassicReport::Button::SELECTDOWN:
                    gp.buttons = gp.buttons | io::GamePadState::Button::SELECT | io::GamePadState::Button::DOWN;
                    break;
                default:
                    break;
                }
                gp.flagConnected(true);
            }
            else
            {
                printf("Invalid PSClassic Mini report size %zd\n", len);
                return;
            }
        }
        else if (isNintendo(vid, pid))
        {
            printf("Nintendo: len = %d\n", len); // Nintendo controllers do not report back
        }
        else
        {
            // ColecoJam: a device whose descriptor did not parse would
            // dereference a null rpt_info_arr below. Fall back to the
            // DirectInput decoder, which is what the pads this project targets
            // actually send.
            if (rpt_count == 0 || rpt_info_arr == nullptr)
            {
                processDirectInputReport(player, report, len);
                if (!tuh_hid_receive_report(dev_addr, instance))
                {
                    printf("Error: cannot request to receive report\r\n");
                }
                return;
            }

            if (rpt_count == 1 && rpt_info_arr[0].report_id == 0)
            {
                // Simple report without report ID as 1st byte
                rpt_info = &rpt_info_arr[0];
            }
            else
            {
                // Composite report, 1st byte is report ID, data starts from 2nd byte
                uint8_t const rpt_id = report[0];

                // Find report id in the arrray
                for (uint8_t i = 0; i < rpt_count; i++)
                {
                    if (rpt_id == rpt_info_arr[i].report_id)
                    {
                        rpt_info = &rpt_info_arr[i];
                        break;
                    }
                }

                report++;
                len--;
            }

            if (!rpt_info)
            {
                printf("Couldn't find the report info for this report !\n");
                return;
            }

            //        printf("usage %d, %d\n", rpt_info->usage_page, rpt_info->usage);

            if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP)
            {
                switch (rpt_info->usage)
                {
                case HID_USAGE_DESKTOP_KEYBOARD:
                {
                    // ColecoJam deviation (a)+(b): a keyboard that exposes no
                    // boot interface reaches us here, having already claimed a
                    // player slot at mount. Hand the slot back and latch it as
                    // the keyboard, so it behaves exactly like a boot-protocol
                    // one: no controller port, merged into port 1 instead.
                    if (len >= sizeof(hid_keyboard_report_t))
                    {
                        if (!kbMounted)
                        {
                            printf("Keyboard detected from report descriptor - device address = %d, instance = %d\n",
                                   dev_addr, instance);
                            auto &gp = io::getCurrentGamePadState(player);
                            gp.flagConnected(false);
                            gp.buttons = 0;
                            gp.GamePadName = nullptr;
                            gp.GamePadShortName = nullptr;
                            unassignPlayer(dev_addr);
                            kbMounted = true;
                            kbDevAddr = dev_addr;
                            kbInstance = instance;
                        }
                        processKeyboardReport(reinterpret_cast<hid_keyboard_report_t const *>(report));
                    }
                    break;
                }

                case HID_USAGE_DESKTOP_MOUSE:
                    TU_LOG1("HID receive mouse report\n");
                    // Assume mouse follows boot report layout (report ID, if
                    // any, has already been stripped above).
                    if (len >= 3)
                    {
                        processMouseReport(reinterpret_cast<hid_mouse_report_t const *>(report), len);
                    }
                    break;

                case HID_USAGE_DESKTOP_JOYSTICK:
                case HID_USAGE_DESKTOP_GAMEPAD:
                {
                    // ColecoJam deviation (c). Upstream reads a 4-byte
                    // {axis[3], buttons} report here and leaves GAMEPAD an
                    // empty stub. The Adafruit SNES-layout pad -- and the cheap
                    // clones most people already own -- send the 8-byte
                    // DirectInput layout instead, with the hat and buttons at
                    // offsets 5 and 6, so decode that whenever the report is
                    // long enough and only fall back to upstream's reading for
                    // shorter ones.
                    if (len >= RPT_MIN_LEN)
                    {
                        processDirectInputReport(player, report, len);
                        break;
                    }

                    struct JoyStickReport
                    {
                        uint8_t axis[3];
                        uint8_t buttons;
                        // 実際のところはしらん
                    };
                    if (len < sizeof(JoyStickReport))
                    {
                        break;
                    }
                    auto *rep = reinterpret_cast<const JoyStickReport *>(report);
                    auto &gp = io::getCurrentGamePadState(player);
                    gp.axis[0] = rep->axis[0];
                    gp.axis[1] = rep->axis[1];
                    gp.axis[2] = rep->axis[2];
                    gp.buttons = rep->buttons;
                    gp.convertButtonsFromAxis(0, 1);
                    gp.flagConnected(true);

                    // BUFFALO BGC-FC801
                    // VID = 0411, PID = 00c6
                }
                break;

                default:
                    break;
                }
            }
        }

        if (!tuh_hid_receive_report(dev_addr, instance))
        {
            printf("Error: cannot request to receive report\r\n");
        }
    }
#pragma region XINPUT
    // Since https://github.com/hathach/tinyusb/pull/2222, we can add in custom vendor drivers easily
    usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
    {
        *driver_count = 1;
        return &usbh_xinput_driver;
    }

    // XXInput type of controlles. (Xbox 360, Xbox One, Xbox Series X)
    // This is somewhat flaky and might not work with all controllers.
    // Tested devices:
    //    - xbox Series X controller : Works
    //    - xbox One controller : Works
    //    - xbox elite controller : Works
    //    - 8bitdo SN30 Pro+ V6.01: Works. Hold X + Start to switch to Xinput mode. (LED 1 and 2 will blink). Then connect to USB.
    //    - 8bitdo Pro 2 V3.04: Works. Hold X + Start to switch to Xinput mode. (LED 1 and 2 will blink). Then connect to USB.
    //    - 8bitdo SN30 PRO Wired : Not working, recognized but no report
    //    - 8bitdo SF30 v2.05 Pro : Works Hold X + Start to switch to Xinput mode. (LED 1 and 2 will blink). Then connect to USB.
    //    - 8bitdo SN30 v2.05 Pro : Not tested, should probably work
    //
    // Troubleshooting:
    //  After flashing some bigger games, the controller might become unresponsive:
    //      - XBOX Controller. Play always with batteries removed. When controller becomes unresponsive:
    //              - unplug and replug the controller.
    //              - If controller is still unresponsive, unplug the pico from power, wait a few seconds then plug it back in.
    //              - Press start on the controller to start the last flashed game.
    //     - 8bitdo controllers, when controller becomes unresponsive:
    //              - Disconnect the controller
    //              - Hold start to switch the controller off (if it has built-in battery).
    //              - reconnect the controller.
    //              - Press start to start the last flashed game.
    void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const *xid_itf, uint16_t len)
    {
        const xinput_gamepad_t *p = &xid_itf->pad;
        const char *type_str;
        int player = getPlayerIndex(dev_addr);
        if (player < 0)
        {
            tuh_xinput_receive_report(dev_addr, instance);
            return;
        }
        isManta[player] = 0;
        if (xid_itf->last_xfer_result == XFER_RESULT_SUCCESS)
        {
#if 0
            switch (xid_itf->type)
            {
            case 1:
                type_str = "Xbox One";
                break;
            case 2:
                type_str = "Xbox 360 Wireless";
                break;
            case 3:
                type_str = "Xbox 360 Wired";
                break;
            case 4:
                type_str = "Xbox OG";
                break;
            default:
                type_str = "Unknown";
            }
#endif
            if (xid_itf->connected && xid_itf->new_pad_data)
            {

                auto &gp = io::getCurrentGamePadState(player);
                gp.buttons = 0;

                if (p->wButtons & XINPUT_GAMEPAD_A)
                    gp.buttons |= (abSwapped ? io::GamePadState::Button::B : io::GamePadState::Button::A);
                if (p->wButtons & XINPUT_GAMEPAD_B)
                    gp.buttons |= (abSwapped ? io::GamePadState::Button::A : io::GamePadState::Button::B);
                if (p->wButtons & XINPUT_GAMEPAD_Y)
                    gp.buttons |= io::GamePadState::Button::X;
                if (p->wButtons & XINPUT_GAMEPAD_X)
                    gp.buttons |= io::GamePadState::Button::Y;
                if (p->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
                    gp.buttons |= io::GamePadState::Button::L;
                if (p->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                    gp.buttons |= io::GamePadState::Button::R;
                if (p->wButtons & XINPUT_GAMEPAD_DPAD_UP)
                    gp.buttons |= io::GamePadState::Button::UP;
                if (p->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
                    gp.buttons |= io::GamePadState::Button::DOWN;
                if (p->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
                    gp.buttons |= io::GamePadState::Button::LEFT;
                if (p->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
                    gp.buttons |= io::GamePadState::Button::RIGHT;
                if (p->wButtons & XINPUT_GAMEPAD_START)
                    gp.buttons |= io::GamePadState::Button::START;
                if (p->wButtons & XINPUT_GAMEPAD_BACK)
                    gp.buttons |= io::GamePadState::Button::SELECT;
                if (p->wButtons & XINPUT_GAMEPAD_GUIDE)
                    gp.buttons |= (io::GamePadState::Button::START | io::GamePadState::Button::SELECT);
                // Left thumbstick doubles as the dpad (sThumbLY is positive up).
                if (p->sThumbLX < -16384)
                    gp.buttons |= io::GamePadState::Button::LEFT;
                else if (p->sThumbLX > 16384)
                    gp.buttons |= io::GamePadState::Button::RIGHT;
                if (p->sThumbLY > 16384)
                    gp.buttons |= io::GamePadState::Button::UP;
                else if (p->sThumbLY < -16384)
                    gp.buttons |= io::GamePadState::Button::DOWN;
                gp.flagConnected(true);
            }
        }
        tuh_xinput_receive_report(dev_addr, instance);
    }

    void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf)
    {

        int player = assignPlayer(dev_addr);
        if (player < 0)
        {
            printf("No free player slot for XINPUT device address = %02x\n", dev_addr);
            return;
        }
        auto &gp = io::getCurrentGamePadState(player);
        // If this is a Xbox 360 Wireless controller we need to wait for a connection packet
        // on the in pipe before setting LEDs etc. So just start getting data until a controller is connected.
        if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false)
        {
            tuh_xinput_receive_report(dev_addr, instance);
            return;
        }
        switch (xinput_itf->type)
        {
        case XBOXONE:
            gp.GamePadName = "Xbox One";
            gp.GamePadShortName = "X1";
            break;
        case XBOX360_WIRELESS:
            gp.GamePadName = "Xbox 360 Wireless";
            gp.GamePadShortName = "X360W";
            break;
        case XBOX360_WIRED:
            gp.GamePadName = "Xbox 360 Wired";
            gp.GamePadShortName = "X360";
            break;
        case XBOXOG:
            gp.GamePadName = "Xbox OG";
            gp.GamePadShortName = "XOG";
            break;
        default:
            gp.GamePadName = "XInput";
            gp.GamePadShortName = "XI";
        }
        printf("XINPUT MOUNTED %02x %d player %d (%s)\n", dev_addr, instance, player + 1, gp.GamePadName);
        tuh_xinput_set_led(dev_addr, instance, 0, true);
        tuh_xinput_set_led(dev_addr, instance, 1, true);
        tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
        tuh_xinput_receive_report(dev_addr, instance);
    }

    void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
    {
        int player = getPlayerIndex(dev_addr);
        if (player >= 0)
        {
            auto &gp = io::getCurrentGamePadState(player);
            gp.GamePadName = nullptr;
            gp.GamePadShortName = nullptr;
            gp.flagConnected(false);
            unassignPlayer(dev_addr);
            printf("XINPUT UNMOUNTED %02x %d player %d\n", dev_addr, instance, player + 1);
        }
        else
        {
            printf("XINPUT UNMOUNTED %02x %d (was not assigned)\n", dev_addr, instance);
        }
    }
#pragma endregion
#ifdef __cplusplus
}
#endif