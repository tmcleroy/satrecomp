// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
//
// Hotkeys for the in-game menu's options (MenuConfig::hotkey, Menu::Listening, Menu::Step): a key or a gamepad
// button given to a feature, which in the game steps it to its next choice (Shift with the key: the one before),
// a switch flipped. Kept by the feature's id (settings.h), as one line of a game's settings:
//   hotkeys=run_ahead:key:F3,display_shader:button:rightstick
// Keys by SDL's names, buttons by SDL's gamepad names, so a line survives SDL's numbering changing.
#pragma once

#include "menu.h"
#include "menu_sdl.h"

#include <SDL3/SDL.h>

#include <map>
#include <string>
#include <vector>

namespace recomp {

struct Hotkey {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;                  // or
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;    //
    bool operator==(const Hotkey &other) const {
        return key == other.key && button == other.button;
    }
    bool Empty() const {
        return key == SDL_SCANCODE_UNKNOWN && button == SDL_GAMEPAD_BUTTON_INVALID;
    }
    // What the menu shows ("F3", "Right stick")
    std::string Name() const {
        if (key != SDL_SCANCODE_UNKNOWN) {
            const char *name = SDL_GetScancodeName(key);
            return name && *name ? name : "?";
        }
        if (button != SDL_GAMEPAD_BUTTON_INVALID) {
            switch (button) {
            case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick";
            case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick";
            case SDL_GAMEPAD_BUTTON_BACK: return "Select";
            case SDL_GAMEPAD_BUTTON_START: return "Start";
            case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
            default: break;
            }
            const char *name = SDL_GetGamepadStringForButton(button);
            return name ? name : "?";
        }
        return "";
    }
};

class Hotkeys {
public:
    // The line kept in the settings, and back (an id or a key not known any more: left out)
    std::string Save() const {
        std::string line;
        for (const auto &[id, hotkey] : m_keys) {
            if (hotkey.Empty()) {
                continue;
            }
            line += (line.empty() ? "" : ",") + id + ":" +
                    (hotkey.key != SDL_SCANCODE_UNKNOWN ? std::string("key:") + SDL_GetScancodeName(hotkey.key)
                                                         : std::string("button:") + SDL_GetGamepadStringForButton(hotkey.button));
        }
        return line;
    }
    void Load(const std::string &line) {
        m_keys.clear();
        size_t at = 0;
        while (at < line.size()) {
            size_t end = line.find(',', at);
            if (end == std::string::npos) {
                end = line.size();
            }
            const std::string item = line.substr(at, end - at);
            at = end + 1;
            const size_t a = item.find(':'), b = a == std::string::npos ? a : item.find(':', a + 1);
            if (b == std::string::npos) {
                continue;
            }
            const std::string id = item.substr(0, a), kind = item.substr(a + 1, b - a - 1), name = item.substr(b + 1);
            Hotkey hotkey;
            if (kind == "key") {
                hotkey.key = SDL_GetScancodeFromName(name.c_str());
            } else if (kind == "button") {
                hotkey.button = SDL_GetGamepadButtonFromString(name.c_str());
            }
            if (!id.empty() && !hotkey.Empty()) {
                m_keys[id] = hotkey;
            }
        }
    }
    Hotkey Of(const std::string &id) const {
        const auto found = m_keys.find(id);
        return found == m_keys.end() ? Hotkey{} : found->second;
    }
    // Given to a feature; taken from any other that had it (one key, one feature)
    void Set(const std::string &id, Hotkey hotkey) {
        for (auto &[other, had] : m_keys) {
            if (had == hotkey) {
                had = {};
            }
        }
        m_keys[id] = hotkey;
    }
    void Clear(const std::string &id) {
        m_keys.erase(id);
    }
    // The feature a key or button is the hotkey of ("": none)
    std::string Find(Hotkey pressed) const {
        if (pressed.Empty()) {
            return "";
        }
        for (const auto &[id, hotkey] : m_keys) {
            if (hotkey == pressed) {
                return id;
            }
        }
        return "";
    }
    bool Any() const {
        for (const auto &[id, hotkey] : m_keys) {
            if (!hotkey.Empty()) {
                return true;
            }
        }
        return false;
    }

private:
    std::map<std::string, Hotkey> m_keys;
};

namespace sdl {

// The key or button of a key or button down event (none for others, a key's repeat, or a key the menu keeps for
// itself: Esc, the menu's own keys)
inline Hotkey HotkeyOf(const SDL_Event &event) {
    Hotkey hotkey;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        hotkey.key = event.key.scancode;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        hotkey.button = static_cast<SDL_GamepadButton>(event.gbutton.button);
    }
    return hotkey;
}

// A key or button the menu itself goes by (menu_sdl.h's MenuHeld, MenuKey), which can't also be a hotkey
inline bool MenuOwn(Hotkey hotkey) {
    switch (hotkey.key) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_Q: case SDL_SCANCODE_E: case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_H: return true;
    case SDL_SCANCODE_UNKNOWN: break;
    default: return MenuKey(hotkey.key);
    }
    switch (hotkey.button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: case SDL_GAMEPAD_BUTTON_DPAD_DOWN: case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_NORTH:
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return true;
    case SDL_GAMEPAD_BUTTON_INVALID: return false;
    default: return MenuButton(hotkey.button);
    }
}

// While the menu asks for a hotkey (Menu::Listening): the event, if it is a key or button, answers it. Esc (or the
// gamepad's B) cancels, Backspace (or X) takes the hotkey away; `taken` says why a key or button cannot be one ("":
// it can; in English: the menu translates it), the menu then saying so and asking again; the menu's own keys and buttons (MenuOwn) never can. `ids`: each menu feature's id. True: the event was the answer's
// (the host does nothing else with it); `changed`: the hotkeys are to be saved.
template <class Taken>
inline bool HotkeyListen(const SDL_Event &event, Menu &menu, Hotkeys &hotkeys, const std::vector<std::string> &ids, Taken taken,
                         bool &changed) {
    const int feature = menu.Listening();
    if (feature < 0) {
        return false;
    }
    const Hotkey pressed = HotkeyOf(event);
    if (pressed.Empty()) {
        // (the mouse cancels, as Esc does)
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            menu.StopListening();
            return true;
        }
        return event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const std::string id = static_cast<size_t>(feature) < ids.size() ? ids[static_cast<size_t>(feature)] : "";
    if (pressed.key == SDL_SCANCODE_ESCAPE || pressed.button == SDL_GAMEPAD_BUTTON_EAST || id.empty()) {
        menu.StopListening();
        return true;
    }
    if (pressed.key == SDL_SCANCODE_BACKSPACE || pressed.button == SDL_GAMEPAD_BUTTON_WEST) {
        hotkeys.Clear(id);
        changed = true;
        menu.StopListening();
        return true;
    }
    const std::string why = MenuOwn(pressed) ? std::string("The menu uses that one.") : taken(pressed);
    if (!why.empty()) {
        menu.ListenNote(why);
        return true;
    }
    hotkeys.Set(id, pressed);
    changed = true;
    menu.StopListening();
    return true;
}

// In the game (the menu closed): a key or button that is a feature's hotkey steps it; what it is now ("Run-ahead:
// 2 frames"), for the host to show, or "" when the event was no hotkey
inline std::string HotkeyPress(const SDL_Event &event, Menu &menu, const Hotkeys &hotkeys, const std::vector<std::string> &ids) {
    const Hotkey pressed = HotkeyOf(event);
    const std::string id = hotkeys.Find(pressed);
    if (id.empty()) {
        return "";
    }
    for (size_t f = 0; f < ids.size(); f++) {
        if (ids[f] == id) {
            const bool back = pressed.key != SDL_SCANCODE_UNKNOWN && (event.key.mod & SDL_KMOD_SHIFT);
            return menu.Step(f, back ? -1 : 1);
        }
    }
    return "";
}

} // namespace sdl
} // namespace recomp
