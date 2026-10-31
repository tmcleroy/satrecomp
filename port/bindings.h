// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Which key and which gamepad button does what, for each player: the launcher's Controllers page writes
// the player's choices to settings.ini and the game reads them. The format and its reading are the recomp
// ports' own (recomp-common/launcher/src/bindings.h); here are this toolkit's actions and its usual layout.
//
//   [keyboard1]         a key by SDL's name for it ("Up", "A", "Return"), or "none"
//   up = Up
//   [gamepad1]          a button or a trigger by SDL's name ("a", "leftshoulder", "righttrigger"), or "none"
//   a = x
//
//   bc = none           a game's combined buttons (game.cfg's `combos`): several Saturn buttons at once
//
// (and [keyboard2], [gamepad2] for player 2). What is not in the file is as the usual layout has it. The
// first gamepad is player 1's and the second player 2's; a gamepad's left stick always works as the
// directions.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../recomp-common/launcher/src/bindings.h"

namespace port {

// What can be bound: the name in settings.ini, what the launcher calls it, and its bit of a Saturn pad
// (kPad* in port.h; frontend_sdl.cpp checks they agree)
struct BoundAction {
    const char *name;
    const char *title;
    uint16_t bit;
};
inline constexpr BoundAction kBoundActions[] = {
    {"up", "Up", 1u << 12},      {"down", "Down", 1u << 13}, {"left", "Left", 1u << 14}, {"right", "Right", 1u << 15},
    {"start", "Start", 1u << 11}, {"a", "A", 1u << 10},      {"b", "B", 1u << 8},        {"c", "C", 1u << 9},
    {"x", "X", 1u << 6},         {"y", "Y", 1u << 5},        {"z", "Z", 1u << 4},        {"l", "L", 1u << 3},
    {"r", "R", 1u << 7},
};
inline constexpr int kBoundActionCount = static_cast<int>(sizeof(kBoundActions) / sizeof(kBoundActions[0]));
inline constexpr int kBoundPlayers = 2;

using recomp::Bindings;
using recomp::PadInput;

// A game's combined buttons, from its game.cfg (`combos`, passed to the build as SATRECOMP_COMBOS): actions that
// press several Saturn buttons at once, for a fighting game's moves that take them together. Each is the
// buttons' names run together, a space and its title, the next after a "|": "bc Punch + kick | abc Guard +
// punch + kick". They have no key or button until the player gives them one.
#ifndef SATRECOMP_COMBOS
#define SATRECOMP_COMBOS ""
#endif
struct ComboAction {
    std::string name, title;
    uint16_t bits = 0;
};
inline std::vector<ComboAction> ParseCombos(const std::string &text) {
    std::vector<ComboAction> combos;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = std::min(text.find('|', at), text.size());
        std::string item = text.substr(at, end - at);
        at = end + 1;
        const size_t first = item.find_first_not_of(' ');
        if (first == std::string::npos) {
            continue;
        }
        item = item.substr(first, item.find_last_not_of(' ') - first + 1);
        const size_t space = item.find(' ');
        ComboAction combo{item.substr(0, space), space == std::string::npos ? item : item.substr(item.find_first_not_of(' ', space)), 0};
        bool known = !combo.name.empty();
        for (const char c : combo.name) {
            bool found = false;
            for (const BoundAction &action : kBoundActions) {
                if (action.name[0] == c && action.name[1] == 0) {
                    combo.bits |= action.bit;
                    found = true;
                }
            }
            known = known && found;
        }
        if (known && static_cast<int>(combos.size()) + kBoundActionCount < recomp::kBindMaxActions) {
            combos.push_back(combo);
        }
    }
    return combos;
}
inline const std::vector<ComboAction> &Combos() {
    static const std::vector<ComboAction> combos = ParseCombos(SATRECOMP_COMBOS);
    return combos;
}
// Every action: the Saturn pad's, then the game's combined buttons
inline int ActionCount() {
    return kBoundActionCount + static_cast<int>(Combos().size());
}
// The Saturn pad bits an action presses
inline uint16_t ActionBits(int action) {
    return action < kBoundActionCount ? kBoundActions[action].bit : Combos()[static_cast<size_t>(action - kBoundActionCount)].bits;
}

// The layout with nothing chosen. Keyboard, player 1: arrows; Return Start; Z X C are A B C; A S D are
// X Y Z; Q E are L R. Player 2 has no keys until some are chosen. A gamepad: the three lower Saturn buttons
// A B C on West, South, East; X Y Z on left shoulder, North, right shoulder; L R on the triggers.
inline const recomp::BindTable &BindingTable() {
    static constexpr SDL_Scancode kKeys[kBoundActionCount] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_Z,  SDL_SCANCODE_X,    SDL_SCANCODE_C,    SDL_SCANCODE_A,     SDL_SCANCODE_S,
        SDL_SCANCODE_D,  SDL_SCANCODE_Q,    SDL_SCANCODE_E,
    };
    static constexpr PadInput kPad[kBoundActionCount] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, -1},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, -1}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, -1},    {SDL_GAMEPAD_BUTTON_START, -1},     {SDL_GAMEPAD_BUTTON_WEST, -1},
        {SDL_GAMEPAD_BUTTON_SOUTH, -1},         {SDL_GAMEPAD_BUTTON_EAST, -1},      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1},
        {SDL_GAMEPAD_BUTTON_NORTH, -1},         {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1},
        {-1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},    {-1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    };
    static const struct Table {
        recomp::BindAction actions[recomp::kBindMaxActions];
        recomp::BindTable table;
        Table() {
            for (int action = 0; action < kBoundActionCount; action++) {
                actions[action] = {kBoundActions[action].name, kBoundActions[action].title, {kKeys[action], SDL_SCANCODE_UNKNOWN}, {kPad[action], kPad[action]}};
            }
            const PadInput none{-1, -1};
            for (size_t i = 0; i < Combos().size(); i++) {
                actions[kBoundActionCount + i] = {Combos()[i].name.c_str(), Combos()[i].title.c_str(), {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN}, {none, none}};
            }
            table = {actions, ActionCount(), kBoundPlayers};
        }
    } table;
    return table.table;
}
inline Bindings DefaultBindings() {
    return recomp::DefaultBindings(BindingTable());
}

// The player's choices from settings.ini (the usual layout where it says nothing, or there is no file)
inline Bindings LoadBindings(const std::string &settingsPath) {
    return recomp::LoadBindings(settingsPath, BindingTable());
}

// Some player has this gamepad input bound (then it does not also do what it does otherwise while playing)
inline bool PadInputBound(const Bindings &b, PadInput input) {
    return recomp::PadInputBound(b, BindingTable(), input);
}

} // namespace port
