// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
//
// Controls on a touch screen (a phone): a stick at the left, the game's buttons at the right, and Start and the
// menu at the top; player 1's, beside a gamepad's and the keys. The buttons are the Saturn's six (X Y Z over A B
// C) and L and R, or the game's own (game.cfg's `touch_buttons`, as `combos` names buttons: "a Guard | b Punch |
// bc P+K", passed to the build as SATRECOMP_TOUCH), a button pressing all the Saturn buttons it names.
// They go where the window has room beside the picture (a phone's screen is wider than it), over its edges where
// it has not. They are shown on Android until a gamepad or a key is used, and anywhere once the screen is touched.
// With no finger on the screen for five seconds they are no longer drawn (the picture whole again); a touch brings
// them back, and is a press of what is under it, as when they are drawn.
// In the menu the stick moves through it and the first two buttons choose and go back.
// (Made from cps2recomp's port/touch.h, which these follow.)
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace port {

class TouchControls {
public:
    TouchControls();
    // The window's size in its pixels, and where the picture is in it
    void Layout(int width, int height, const SDL_FRect &picture);
    // An event of the window's: a finger's is the controls' (true); a gamepad's or a key's hides them
    bool Event(const SDL_Event &event);
    bool Shown() const {
        return m_shown;
    }
    // Player 1's controls held (the Saturn pad's bits, port.h's kPad*), read once a frame: a press lasts a few
    // frames however short the tap; and the menu's choose and back (recomp::kMenu* bits) as last read. With the
    // menu open, the stick held in one way goes on through its rows, as a held key does in a list.
    uint16_t Read(bool menuOpen);
    unsigned Menu() const;
    // The menu button was pressed since this was last asked
    bool TakeMenuPress();
    // Their picture, when it changed since this was last asked (and for nothing shown, an empty one): half the
    // window's size, its alpha premultiplied, to be stretched over the whole window
    bool TakePicture(std::vector<uint32_t> &pixels, int &width, int &height);

private:
    struct Control {
        float x, y, radius; // in the window's pixels
        uint16_t bits;      // what it presses (none for the menu button)
        std::string label;
        int colour = -1;    // (the face buttons': which of the three colours)
        bool menu = false;
    };
    struct Button {
        uint16_t bits;
        std::string label;
    };
    int Hit(float x, float y) const;
    void Changed() {
        m_changed = true;
    }
    void Draw();

    bool m_shown = false, m_changed = true, m_menuPressed = false;
    bool m_idle = false;    // (shown, but no finger for a while: not drawn)
    uint64_t m_touched = 0; // (when a finger was last on the screen, SDL_GetTicks)
    int m_width = 0, m_height = 0;
    SDL_FRect m_picture{};
    float m_stickX = 0, m_stickY = 0, m_stickRadius = 0; // the stick's middle and reach
    float m_knobX = 0, m_knobY = 0;                      // where its finger is
    std::vector<Control> m_controls;
    std::vector<Button> m_buttons;   // the game's (touch_buttons), or none: the Saturn's own
    struct Finger {
        int control; // the stick (-1) or a control
        int reads;   // frames it has been read in
    };
    std::map<SDL_FingerID, Finger> m_fingers;            // each finger down
    std::vector<std::pair<uint16_t, int>> m_lingering;   // a short tap's: its bits, and the frames still to give them
    uint16_t m_stick = 0, m_read = 0, m_stickBefore = 0;
    int m_stickHeld = 0; // the frames the stick has been held the same way
    std::vector<uint32_t> m_pixels;
    int m_pictureWidth = 0, m_pictureHeight = 0;
};

} // namespace port
