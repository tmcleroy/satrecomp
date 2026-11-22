// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// The in-game menu's controls, the same in every port on SDL 3: what opens and closes it, its buttons on the
// keyboard and gamepads, and the mouse.
//
//   Open and close (from any page): Esc, F1, a phone's Back, Tab (unless the game has it); a gamepad's Guide, its
//     left stick's click, or Start and Back (Select) together.
//   In the menu: the arrows, the d-pad and the left stick choose (held: on and on, with MenuRepeat); Enter, Space
//     and A (south) confirm; Backspace and B (east) go back a page. The mouse: over a row chooses it, a click
//     confirms, the right button goes back, the wheel goes up and down.
//
// Header only; the port includes it where it has SDL 3, and links SDL as it does already.
#pragma once

#include "menu.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_touch.h>
#include <SDL3/SDL_video.h>

namespace recomp::sdl {

// The hint line's names for the buttons (MenuConfig::confirmName and closeName)
constexpr const char *kConfirmName = "Enter or A";
constexpr const char *kCloseName = "Esc";

// A key that opens and closes the menu (tabFree: the game has not been given Tab)
inline bool MenuKey(SDL_Scancode key, bool tabFree = true) {
    return key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_F1 || key == SDL_SCANCODE_AC_BACK || (tabFree && key == SDL_SCANCODE_TAB);
}

// A gamepad button that opens and closes the menu by itself (one the game has not been given)
inline bool MenuButton(SDL_GamepadButton button) {
    return button == SDL_GAMEPAD_BUTTON_GUIDE || button == SDL_GAMEPAD_BUTTON_LEFT_STICK;
}

// Start and Back held together on a gamepad, whatever the game has them as: Step is true when they have just come
// to be held together (once, until one is let go)
struct MenuChord {
    bool held = false;
    bool Step(SDL_Gamepad *const *pads, int count) {
        bool both = false;
        for (int i = 0; i < count; i++) {
            both = both || (pads[i] && SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_START) &&
                            SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_BACK));
        }
        const bool now = both && !held;
        held = both;
        return now;
    }
};

// The menu's buttons held now (kMenu... bits) on the keyboard (SDL_GetKeyboardState) and the gamepads: give them to
// MenuRepeat::Step each frame, and what it returns to Menu::Buttons. A port adds its game's own directions if it
// likes (the player's keys for up and down, say).
inline unsigned MenuHeld(const bool *keys, SDL_Gamepad *const *pads, int count) {
    unsigned held = 0;
    if (keys) {
        held |= keys[SDL_SCANCODE_UP] ? kMenuUp : 0;
        held |= keys[SDL_SCANCODE_DOWN] ? kMenuDown : 0;
        held |= keys[SDL_SCANCODE_LEFT] ? kMenuLeft : 0;
        held |= keys[SDL_SCANCODE_RIGHT] ? kMenuRight : 0;
        // (Alt+Enter is full screen, not Confirm)
        held |= keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_KP_ENTER] || (keys[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT))
                    ? kMenuConfirm
                    : 0;
        held |= keys[SDL_SCANCODE_BACKSPACE] ? kMenuBack : 0;
    }
    constexpr Sint16 kTilt = 16384; // (half way)
    for (int i = 0; i < count; i++) {
        SDL_Gamepad *pad = pads[i];
        if (!pad) {
            continue;
        }
        const Sint16 x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kTilt ? kMenuUp : 0;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kTilt ? kMenuDown : 0;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kTilt ? kMenuLeft : 0;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kTilt ? kMenuRight : 0;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ? kMenuConfirm : 0;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ? kMenuBack : 0;
    }
    return held;
}

// Where the menu's picture is on the window: its rectangle in the window's pixels (as the port draws it there),
// and the picture's own size (what Menu::Draw was given)
struct MenuPlace {
    float x = 0, y = 0, w = 0, h = 0;
    int width = 0, height = 0;
};

// The mouse, for the menu: an SDL event of the mouse's, while the menu is open, handed on to Menu::Mouse. True when
// the event was the mouse's (so the picture is to be drawn again); `chosen` is what the menu returned. A touch the
// system has also made into a mouse event is left to the port's touch controls.
inline bool MouseEvent(const SDL_Event &event, SDL_Window *window, const MenuPlace &place, Menu &menu, Menu::Chosen &chosen) {
    chosen = {};
    float px = 0, py = 0;
    MenuMouse what;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which == SDL_TOUCH_MOUSEID) {
            return false;
        }
        what = MenuMouse::Move;
        px = event.motion.x, py = event.motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.which == SDL_TOUCH_MOUSEID || (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_RIGHT)) {
            return false;
        }
        what = event.button.button == SDL_BUTTON_LEFT ? MenuMouse::Click : MenuMouse::Back;
        px = event.button.x, py = event.button.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.which == SDL_TOUCH_MOUSEID || event.wheel.y == 0) {
            return false;
        }
        what = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y) > 0 ? MenuMouse::WheelUp : MenuMouse::WheelDown;
        px = event.wheel.mouse_x, py = event.wheel.mouse_y;
        break;
    default: return false;
    }
    if (!menu.IsOpen() || place.w <= 0 || place.h <= 0) {
        return false;
    }
    // (the window's points to its pixels, then to the picture's)
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    const float x = (px * (density > 0 ? density : 1.0f) - place.x) * place.width / place.w;
    const float y = (py * (density > 0 ? density : 1.0f) - place.y) * place.height / place.h;
    chosen = menu.Mouse(what, x, y);
    return true;
}

} // namespace recomp::sdl
