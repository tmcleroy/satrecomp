// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// The menu turned with the picture: for a game made for a monitor on its side, shown as the board sends it on a
// monitor the player has turned on its side (tate), the menu and what the port writes over the picture (its notes,
// readouts, the input display) are turned too, so they read upright on that monitor.
//
// A port with a rotated-screen option gives the player the Menu orientation feature (kTitle, kDescription,
// Choices(): Auto, Upright and the three turns) on its Display page, and each time it draws:
//
//   - MenuTurn(choice, pictureTurn, uprightTurn): the menu's turn on the screen, in degrees clockwise (0, 90, 180 or
//     270). Auto: as the game's picture stands (pictureTurn, the picture's turn on the screen from the board's own,
//     less uprightTurn, the turn that stands the game up), so whoever sees the picture upright reads the menu upright.
//   - TurnedSize: the size of the menu's own picture for a turn (the picture over which it is drawn, turned back),
//     which is what Menu::Draw is given: the menu lays itself out for the turned screen's width and height.
//   - TurnPixels: that picture turned onto the screen's.
//   - sdl::MenuPlace::turn (menu_sdl.h): the mouse and the fingers mapped back through the turn (Unturn).
//
// The gamepad's and the keys' directions need nothing: the player holds them upright, as the menu now stands, so up
// on the stick goes up the list as the player sees it.
//
// Header only, C++17, no dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::turn {

// The feature's words, the same in every port that has it
constexpr const char *kTitle = "Menu orientation";
constexpr const char *kDescription =
    "Which way up the menu stands, and the notes and readouts drawn over the picture. Auto: as the game's picture "
    "stands, so with the picture turned for a monitor on its side (As the cabinet) they turn with it and read upright "
    "on that monitor. Upright: always upright on the screen as it is, whichever way the picture is turned. Top to the "
    "right, Upside down and Top to the left: turned that way on the screen, for a monitor turned some other way.";

// Its choices, in this order (Auto, the default, first)
enum Choice { kAuto = 0, kUpright, kTopRight, kUpsideDown, kTopLeft };
inline std::vector<std::string> Choices() {
    return {"Auto", "Upright", "Top to the right", "Upside down", "Top to the left"};
}

// Degrees made 0, 90, 180 or 270 (a multiple of 90 assumed)
inline int Normal(int degrees) {
    return ((degrees % 360) + 360) % 360 / 90 * 90;
}

// The menu's turn on the screen, in degrees clockwise, for the player's `choice`: pictureTurn is the turn of the
// picture on the screen from the board's own (as the port shows it), uprightTurn the turn that stands the game's
// picture up as it was made to be seen. Auto: their difference (0 whenever the picture is shown stood up)
inline int MenuTurn(int choice, int pictureTurn, int uprightTurn) {
    switch (choice) {
    case kUpright: return 0;
    case kTopRight: return 90;
    case kUpsideDown: return 180;
    case kTopLeft: return 270;
    default: return Normal(pictureTurn - uprightTurn);
    }
}

// The size of a w x h picture turned by `turn` degrees (either way: a quarter turn swaps them)
inline void TurnedSize(int turn, int w, int h, int &tw, int &th) {
    const bool quarter = Normal(turn) % 180 != 0;
    tw = quarter ? h : w;
    th = quarter ? w : h;
}

// A w x h picture turned `turn` degrees clockwise into `out` (TurnedSize's size; not the same memory as `in`)
inline void TurnPixels(const uint32_t *in, int w, int h, int turn, uint32_t *out) {
    const size_t W = static_cast<size_t>(w), H = static_cast<size_t>(h);
    switch (Normal(turn)) {
    case 90: // (the top to the right: the out picture is h wide)
        for (size_t y = 0; y < H; y++) {
            for (size_t x = 0; x < W; x++) {
                out[x * H + (H - 1 - y)] = in[y * W + x];
            }
        }
        break;
    case 180:
        for (size_t i = 0, n = W * H; i < n; i++) {
            out[n - 1 - i] = in[i];
        }
        break;
    case 270: // (the top to the left)
        for (size_t y = 0; y < H; y++) {
            for (size_t x = 0; x < W; x++) {
                out[(W - 1 - x) * H + y] = in[y * W + x];
            }
        }
        break;
    default:
        for (size_t i = 0, n = W * H; i < n; i++) {
            out[i] = in[i];
        }
        break;
    }
}

// A place on a picture turned by `turn` degrees clockwise (u, v: fractions of its width and height) back on the
// picture before the turn (x, y: fractions of its width and height). For the mouse and the fingers
inline void Unturn(int turn, float u, float v, float &x, float &y) {
    switch (Normal(turn)) {
    case 90: x = v; y = 1 - u; break;
    case 180: x = 1 - u; y = 1 - v; break;
    case 270: x = 1 - v; y = u; break;
    default: x = u; y = v; break;
    }
}

} // namespace recomp::turn
