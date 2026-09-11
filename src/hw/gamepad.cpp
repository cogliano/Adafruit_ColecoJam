// gamepad.cpp -- storage for the normalised controller state
//
// Ported from pico-infonesPlus (pico_shared/gamepad.cpp), original author
// Shuichi TAKANO. See gamepad.h for the deviations.
//
// No locking: hid_app.cpp writes these from tuh_task(), and usb_host.cpp reads
// them from usb_host_update_coleco(). Both run on whichever core owns the host
// stack (core 0 under VIDEO_DRIVER_PICO_HDMI, core 1 otherwise), never both.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gamepad.h"

namespace io
{
    namespace
    {
        GamePadState currentGamePad_[2];
        KeyboardState currentKeyboard_;
        MouseState currentMouse_;
    }

    GamePadState &getCurrentGamePadState(int i)
    {
        return currentGamePad_[i];
    }

    const KeyboardState &getCurrentKeyboardState()
    {
        return currentKeyboard_;
    }

    KeyboardState &keyboardState()
    {
        return currentKeyboard_;
    }

    MouseState &getCurrentMouseState()
    {
        return currentMouse_;
    }

    void
    GamePadState::convertButtonsFromAxis(int axisX, int axisY)
    {
        int x = axis[axisX];
        int y = axis[axisY];

        if (x < 64)
        {
            buttons |= Button::LEFT;
        }
        else if (x > 192)
        {
            buttons |= Button::RIGHT;
        }

        if (y < 64)
        {
            buttons |= Button::UP;
        }
        else if (y > 192)
        {
            buttons |= Button::DOWN;
        }
    }

    void
    GamePadState::convertButtonsFromHat()
    {
        static constexpr int table[] = {
            Button::UP,
            Button::UP | Button::RIGHT,
            Button::RIGHT,
            Button::DOWN | Button::RIGHT,
            Button::DOWN,
            Button::LEFT | Button::DOWN,
            Button::LEFT,
            Button::LEFT | Button::UP,
        };
        auto i = static_cast<int>(hat);
        if (i < 8)
        {
            buttons |= table[i];
        }
    }
}
