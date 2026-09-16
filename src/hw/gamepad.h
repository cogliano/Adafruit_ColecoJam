// gamepad.h -- normalised controller / keyboard / mouse state
//
// Ported from pico-infonesPlus (pico_shared/gamepad.h), original author
// Shuichi TAKANO. hid_app.cpp is the only writer; usb_host.cpp is the only
// reader, and translates this into ColecoVision controller state.
//
// Deviations from upstream, both driven by ColecoJam treating a keyboard as a
// companion to a gamepad rather than as a controller in its own right:
//
//   * KeyboardState gains `connected` and `buttons`. Upstream borrows a
//     GamePadState slot for the keyboard's joystick-style mapping, which would
//     cost us a controller port; here that mapping lands in `buttons` and the
//     keyboard claims no slot at all.
//   * keyboardState() exposes a non-const reference for hid_app.cpp, so it does
//     not have to const_cast getCurrentKeyboardState() the way upstream does.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

namespace io
{
    struct GamePadState
    {
        bool connected{false};
        struct Button
        {
            inline static constexpr int A = 1 << 0;
            inline static constexpr int B = 1 << 1;
            inline static constexpr int X = 1 << 2;
            inline static constexpr int Y = 1 << 3;
            inline static constexpr int C = 1 << 4;
            inline static constexpr int Z = 1 << 5;
            inline static constexpr int SELECT = 1 << 6;
            inline static constexpr int START = 1 << 7;
            inline static constexpr int L = 1 << 8;
            inline static constexpr int R = 1 << 9;

            inline static constexpr int LEFT = 1 << 31;
            inline static constexpr int RIGHT = 1 << 30;
            inline static constexpr int UP = 1 << 29;
            inline static constexpr int DOWN = 1 << 28;
        };

        enum class Hat
        {
            N,
            NE,
            E,
            SE,
            S,
            SW,
            W,
            NW,
            RELEASED,
        };

        uint8_t axis[3]{0x80, 0x80, 0x80};
        Hat hat{Hat::RELEASED};
        uint32_t buttons{0};

    public:
        void convertButtonsFromAxis(int axisX, int axisY);
        void convertButtonsFromHat();
        void flagConnected(bool connected) { this->connected = connected; }
        bool isConnected() const { return connected; }
        const char *GamePadName{nullptr};
        const char *GamePadShortName{nullptr};
    };

    GamePadState &getCurrentGamePadState(int i);

    // Raw HID keyboard state, populated from the most recent boot-protocol
    // keyboard report. `modifier` matches HID_KEYBOARD_MODIFIER_* and
    // `keycode[]` holds the up to 6 currently-pressed HID usage codes
    // (HID_KEY_*) -- usb_host.cpp reads those directly to drive the
    // ColecoVision keypad, which needs the digits and Shift state that a
    // button bitmask cannot carry.
    //
    // `buttons` additionally carries the joystick-style mapping (arrows, Z/X,
    // Q/W, A, S, Enter) as GamePadState::Button bits, so a keyboard can drive
    // the ROM browser and the joystick without occupying a controller port.
    struct KeyboardState
    {
        bool connected{false};
        uint8_t modifier{0};
        uint8_t keycode[6]{};
        uint32_t buttons{0};
    };
    const KeyboardState &getCurrentKeyboardState();
    KeyboardState &keyboardState();          // writable, for hid_app.cpp

    // USB HID mouse state, from boot-protocol mouse reports. Movement and wheel
    // are accumulated deltas: the consumer should read them and reset
    // dx/dy/wheel to zero after processing (single-consumer model). `buttons`
    // matches MOUSE_BUTTON_* from TinyUSB (bit 0 left, 1 right, 2 middle).
    //
    // Nothing in ColecoJam consumes this yet -- it is carried over so the
    // ported hid_app.cpp stays close to upstream.
    struct MouseState
    {
        bool connected{false};
        uint8_t buttons{0};
        int32_t dx{0};
        int32_t dy{0};
        int32_t wheel{0};
    };
    MouseState &getCurrentMouseState();
}
