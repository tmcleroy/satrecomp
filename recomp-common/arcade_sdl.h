// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// An arcade monitor on SDL 3: full screen in the board's own resolution, or in a super resolution, on the screen the
// player chose. The same in every arcade port (taken from cps2recomp's, which took it from sf3comp's).
//
//   Full screen resolution (ArcadeMode):
//     Off    the desktop's own, as ever.
//     Board  a mode of exactly the board's columns and lines (384 x 224, say: CRT Emudriver's, or a custom
//            resolution), the picture pixel for pixel.
//     Super  a mode of the board's lines and 1920 columns or more (2560 x 224), the picture stretched across: the lines
//            one for one, and the monitor blends the columns so each comes out the same width, which a card can't do
//            at 384. Where the screen has none of the board's lines and the board has fewer than 240, one of 240
//            (2560 x 240: not every card's set goes below 240p), the picture's lines one for one in its middle
//            (ArcadeLines).
//     Either way only a mode at the refresh nearest the board's and no further than 5 Hz from it; with none, full
//     screen stays at the desktop's resolution and the log says why. Never another number of lines or another rate: a
//     monitor of 15 kHz can be harmed by a mode it cannot take.
//   Screen (0-3): the main screen, or the second, third or fourth in the order SDL lists them; the main one where the
//     one chosen isn't connected. The window opens centred on it and goes full screen there.
//
// The port keeps the choices as its features (set on the launcher: a mode the monitor may not take is never chosen
// while playing; the menu can only put them back to off), passes them here, and on an arcade mode fills the screen
// with the picture (the monitor gives it its shape) and makes the menu compact (Menu::SetCompact).
//
// Header only; the port includes it where it has SDL 3, and links SDL as it does already.
#pragma once

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace recomp::sdl {

enum class ArcadeMode { Off, Board, Super };

// The board's picture: its columns and lines, and its frame rate
struct ArcadeBoard {
    int width;
    int lines;
    double rate;
};

// The choices' names, for the port's feature ("Off", "384 x 224", "Super resolution"), and its description
inline std::string ArcadeBoardName(const ArcadeBoard &board) {
    return std::to_string(board.width) + " x " + std::to_string(board.lines);
}
inline std::string ArcadeDescription(const ArcadeBoard &board) {
    char rate[16];
    snprintf(rate, sizeof rate, "%.2f", board.rate);
    const std::string lines = std::to_string(board.lines);
    std::string text = "For an arcade monitor (CRT Emudriver or a custom resolution). " + ArcadeBoardName(board) +
                       ": full screen switches to the board's own resolution, pixel for pixel. Super resolution: to one of "
                       "its " + lines + " lines and 1920 or more columns (2560 x " + lines + ", say), the picture stretched across";
    if (board.lines < 240) {
        text += "; with none, one of 240 lines (2560 x 240), the " + lines + " in its middle";
    }
    return text + ". Only such a mode near " + rate + " Hz is used; if there is none, full screen stays as it was. Set on the launcher.";
}
constexpr const char *kScreenDescription = "Which screen the game opens on and goes full screen on, where there are several: the main "
                                           "one or another, in the order the system lists them. One that isn't connected: the main. "
                                           "Set on the launcher; the in-game menu can only put it back to the main one.";
constexpr const char *kCompactDescription = "The in-game menu in small letters with its rows close together, as suits a screen of few "
                                            "lines (a small window, an arcade monitor): a page's rows all shown at once, none running "
                                            "into the next. Off: compact only in full screen at an arcade resolution.";

constexpr int kSuperTallLines = 240; // (a super resolution of 240p, where the card has none of the board's lines)
constexpr int kSuperLeastColumns = 1920;
constexpr float kArcadeRateRoom = 5.0f; // (Hz either side of the board's)

// The best of the modes for `want`, or -1 (a mode's size in pixels: its points times its density). Pure, for the tests:
// a mode of the board's lines before one of 240; then the refresh nearest the board's; then the widest
inline int ArcadeModeIndex(const SDL_DisplayMode *const *modes, int count, ArcadeMode want, const ArcadeBoard &board) {
    int best = -1;
    long bestLines = 0;
    float bestOff = 0;
    int bestWidth = 0;
    const float rate = static_cast<float>(board.rate);
    for (int i = 0; modes && i < count && want != ArcadeMode::Off; i++) {
        const SDL_DisplayMode &mode = *modes[i];
        const float density = mode.pixel_density > 0 ? mode.pixel_density : 1.0f;
        const long w = std::lround(mode.w * density), h = std::lround(mode.h * density);
        const bool size = want == ArcadeMode::Super
                              ? (h == board.lines || (board.lines < kSuperTallLines && h == kSuperTallLines)) && w >= kSuperLeastColumns
                              : h == board.lines && w == board.width;
        const float off = std::fabs(mode.refresh_rate - rate);
        if (!size || off > kArcadeRateRoom) {
            continue;
        }
        const bool better = best < 0 || (h == board.lines && bestLines != board.lines) ||
                            (h == bestLines && (off < bestOff - 0.01f || (std::fabs(off - bestOff) <= 0.01f && mode.w > bestWidth)));
        if (better) {
            best = i;
            bestLines = h;
            bestOff = off;
            bestWidth = mode.w;
        }
    }
    return best;
}

// The screen chosen (0: the main display; 1-3: the others in the order SDL lists them); the main one where the one
// chosen is not connected
inline SDL_DisplayID ChosenDisplay(int screen) {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_DisplayID chosen = primary;
    int count = 0, other = 0;
    SDL_DisplayID *all = SDL_GetDisplays(&count);
    for (int i = 0; all && i < count; i++) {
        if (all[i] != primary && ++other == screen) {
            chosen = all[i];
        }
    }
    SDL_free(all);
    return chosen;
}

// The screen's mode for `want`; false: it has none
inline bool ArcadeModeOf(SDL_DisplayID display, ArcadeMode want, const ArcadeBoard &board, SDL_DisplayMode &found) {
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count);
    const int at = ArcadeModeIndex(modes, count, want, board);
    if (at >= 0) {
        found = *modes[at];
    }
    SDL_free(modes);
    return at >= 0;
}

// The window's full screen as chosen: the arcade mode on the chosen screen where it has one; else the desktop's own.
// Kept by the window for F11 and Alt+Enter too
inline void SetFullscreenMode(SDL_Window *window, ArcadeMode want, int screen, const ArcadeBoard &board) {
    SDL_DisplayMode mode{};
    if (want != ArcadeMode::Off && ArcadeModeOf(ChosenDisplay(screen), want, board, mode)) {
        if (!SDL_SetWindowFullscreenMode(window, &mode)) {
            fprintf(stderr, "full screen %dx%d at %.2f Hz: %s\n", mode.w, mode.h, mode.refresh_rate, SDL_GetError());
        }
        return;
    }
    if (want != ArcadeMode::Off) {
        if (want == ArcadeMode::Super) {
            fprintf(stderr, "full screen: the screen has no super resolution (%d columns or more) of %d lines%s near %.2f Hz; full screen stays at its own resolution\n",
                    kSuperLeastColumns, board.lines, board.lines < kSuperTallLines ? " or of 240" : "", board.rate);
        } else {
            fprintf(stderr, "full screen: the screen has no %dx%d mode near %.2f Hz; full screen stays at its own resolution\n",
                    board.width, board.lines, board.rate);
        }
    }
    SDL_SetWindowFullscreenMode(window, nullptr);
}

// The window in the middle of the chosen screen
inline void CentreOnScreen(SDL_Window *window, int screen) {
    const SDL_DisplayID display = ChosenDisplay(screen);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
}

// Whether the window is full screen in a mode of its own (an arcade resolution): the picture then fills the screen,
// whose shape the monitor gives it
inline bool OnArcadeMode(SDL_Window *window) {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) && SDL_GetWindowFullscreenMode(window);
}

// Where the picture goes on an arcade mode of more lines than the board's (a super resolution of 240): its lines one
// for one in the middle, the lines above and below black, never stretched. Otherwise `where` as it is
inline SDL_FRect ArcadeLines(SDL_FRect where, int screenLines, int boardLines) {
    if (screenLines > boardLines && screenLines < 2 * boardLines) {
        where.h = static_cast<float>(boardLines);
        where.y = std::floor((screenLines - boardLines) / 2.0f);
    }
    return where;
}

// The Screen and the Full screen resolution changed (the menu can put them back to off): the window to that screen,
// full screen in that mode. The port calls it each frame with what is chosen now
struct ArcadeFollow {
    bool started = false;
    int screen = 0;
    ArcadeMode mode = ArcadeMode::Off;
    void Step(SDL_Window *window, ArcadeMode want, int screenNow, const ArcadeBoard &board) {
        if (!started) {
            started = true;
            screen = screenNow;
            mode = want;
            return;
        }
        if (!window || (screenNow == screen && want == mode)) {
            return;
        }
        const bool full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
        if (full) {
            SDL_SetWindowFullscreen(window, false);
            SDL_SyncWindow(window);
        }
        if (screenNow != screen) {
            CentreOnScreen(window, screenNow);
        }
        SetFullscreenMode(window, want, screenNow, board);
        if (full) {
            SDL_SetWindowFullscreen(window, true);
        }
        SDL_SyncWindow(window);
        screen = screenNow;
        mode = want;
    }
};

} // namespace recomp::sdl
