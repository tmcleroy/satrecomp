// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// The rest of an arcade monitor (arcade_sdl.h is its modes and screen): a mode made at the board's exact rate with
// Switchres, the frames synced to the screen's refreshes, and a frame delay. The same in every arcade port (taken from
// cps2recomp's: its PRs #417 and #420).
//
//   Make the mode at the board's rate (Windows): with the Full screen resolution at the board's or Super, full screen has
//     Switchres (GroovyMAME's modeline generator, GPL-2.0-or-later, downloaded at build time: switchres.cmake) make that
//     mode at exactly the board's rate within the Monitor preset's ranges, switch the desktop to it, and go full screen
//     on that desktop; the desktop comes back at the end. On failure: the modes the system lists, as without it.
//   Keep the board's pace (off by default): off, in full screen at an arcade mode the frames are paced by the screen's
//     refreshes (VSync on, one frame a refresh), as a mode the system lists is seldom exactly the board's rate; on, by the
//     board's clock, as in a window. Online always the board's pace.
//   The frame delay (synced, Windows, where the system says when the screen refreshes): each frame's controls read and
//     its picture made as late before the refresh it goes up at as its recent work and a margin allow.
//
// The port keeps the choices as features (arcade_sdl.h has their names and descriptions), and has one ArcadeScreen:
//   SetFullscreen when the window is made and when the choices change (Follow each frame does that), OnArcade for the
//   picture's place and the compact menu, Pace each frame (Paced says whether to pace by the screen; WorkDone just
//   before the picture is shown; Shown just after it, which waits as the frame delay says), Menu while the in-game menu
//   is up, and Close at the end (the desktop's mode as it was).
//
// The port compiles arcade_screen.cpp, and on Windows links Switchres: include(recomp-common/switchres.cmake) and
// recomp_switchres(<target>).
#pragma once

#include "arcade_sdl.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace recomp::sdl {

// Switchres's monitor presets, in the order of the Monitor choice's names (arcade_sdl.h's kMonitorNames)
constexpr const char *kMonitorPresets[] = {"arcade_15", "generic_15", "ntsc", "pal", "arcade_15_25", "arcade_15_25_31", "arcade_31"};
constexpr int kMonitorPresetCount = sizeof kMonitorPresets / sizeof kMonitorPresets[0];

// A mode Switchres made and switched the desktop to
struct MadeMode {
    int width = 0, height = 0;
    double vfreq = 0, hfreq = 0; // (hertz)
    bool exact = false;          // its refresh is the one asked for (else the nearest the monitor's ranges allow)
};
// Whether this build has Switchres (Windows)
bool SwitchresBuilt();
// The desktop of the screen `screen` (0 the main one, then the order the system lists them) switched to a mode of
// `width` x `height` at `rate` hertz, made by Switchres for the monitor `monitor` (one of kMonitorPresets); false and
// `error` if it could not be
bool SwitchresMake(int screen, const char *monitor, int width, int height, double rate, MadeMode &made, std::string &error);
// Whether the desktop is in a mode Switchres made
bool SwitchresActive();
// The desktop as it was, and Switchres's modes taken back
void SwitchresRestore();

// The screen's refreshes, as the system says they happen (Windows: D3DKMTWaitForVerticalBlankEvent, on a thread of its
// own). Started for the screen a window is on; elsewhere, or where the system can't say, never Valid
class RefreshClock {
public:
    RefreshClock() = default;
    RefreshClock(const RefreshClock &) = delete;
    RefreshClock &operator=(const RefreshClock &) = delete;
    ~RefreshClock();
    void Start(SDL_Window *window);
    void Stop();
    // The time (SDL_GetTicksNS) of the refresh that follows `when`, and the time between refreshes; false while the
    // clock has seen too few of them to say
    bool Next(uint64_t when, uint64_t &next, double &period) const;

private:
    struct Impl;
    [[maybe_unused]] Impl *m_impl = nullptr;
};

// The player's Arcade monitor choices, as the port's features hold them now
struct ArcadeChoices {
    ArcadeMode mode = ArcadeMode::Off; // Full screen resolution
    int screen = 0;                    // Screen
    bool switchres = false;            // Make the mode at the board's rate (only where SwitchresBuilt)
    int monitor = 0;                   // Monitor (an index of kMonitorPresets)
    bool boardPace = false;            // Keep the board's pace
};

class ArcadeScreen {
public:
    explicit ArcadeScreen(const ArcadeBoard &board) : m_board(board) {}
    ArcadeScreen(const ArcadeScreen &) = delete;
    ArcadeScreen &operator=(const ArcadeScreen &) = delete;
    ~ArcadeScreen() { Close(); }

    // The window's full screen as chosen: with Make the mode at the board's rate and the window going full screen
    // (`full`), the desktop switched to Switchres's mode and the window full screen on that desktop; else the chosen
    // screen's listed arcade mode (arcade_sdl.h's SetFullscreenMode). Kept by the window for F11 and Alt+Enter too
    void SetFullscreen(SDL_Window *window, const ArcadeChoices &choices, bool full);
    // Each frame, with the choices as they are now: the window to the screen and mode chosen, where they changed (the
    // menu can put them back to off)
    void Follow(SDL_Window *window, const ArcadeChoices &choices);
    // Whether the window is full screen in an arcade mode: one it was given, or the desktop in the mode Switchres made
    bool OnArcade(SDL_Window *window) const;

    // Whether this frame is paced by the screen (full screen at an arcade mode, Keep the board's pace off, not online):
    // the port then turns VSync on and calls Shown instead of keeping its own frame timer. Starts and stops the refresh
    // clock as it changes
    bool Paced(SDL_Window *window, const ArcadeChoices &choices, bool online);
    // This frame's work done (the game run; its picture about to be drawn and shown). The frame delay measures up to here
    void WorkDone();
    // Synced, just after the picture is shown: waits as the frame delay says (or, where the refreshes aren't known, no
    // sooner than nine tenths of a board's frame after the last). Returns the time the next frame starts
    uint64_t Shown();
    // Not synced, on the board's clock: in an arcade mode, the rate as measured goes in the log
    void BoardPace(bool arcade);
    // The in-game menu up: its time is not the game's (the rates measured, and the frame delay's last frame, start again)
    void Menu();
    // The end: the refresh clock stopped, the desktop's mode as it was
    void Close();

private:
    ArcadeBoard m_board;
    // (Follow's last choices)
    bool m_started = false;
    ArcadeChoices m_followed;
    // (Paced's refresh clock, and the window size it was started at)
    RefreshClock m_clock;
    bool m_clockOn = false;
    int m_clockWidth = 0, m_clockHeight = 0;
    // (the frame delay, in nanoseconds)
    uint64_t m_syncLast = 0, m_syncSince = 0; // (the last frame's time, the measuring's start)
    int m_syncFrames = 0;
    double m_work = 0, m_margin = 1.5e6, m_delaySum = 0;
    uint64_t m_aimed = 0;   // (the refresh the frame being made was to be ready for)
    uint64_t m_workEnd = 0; // (when this frame's work was done: the wait for the screen not counted)
    int m_missed = 0, m_calm = 0;
    // (the board's pace in an arcade mode: the rate as measured, for the log)
    uint64_t m_paceSince = 0;
    int m_paceFrames = 0;
};

} // namespace recomp::sdl
