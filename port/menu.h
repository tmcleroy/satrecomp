// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The in-game menu, while playing: save states (save, load, slot), the game's features, a page for each of
// their categories, the game's speed and the reset point (for practice), and leaving the game. The frontend
// opens it, stops the game while it is open and gives it the pad's presses; it draws itself over a copy of
// the game's picture.
//
// The menu itself is the one the recomp ports share (recomp-common/menu.h, a copy of recomp-common's: see
// tools/sync-common.sh). This is what is the Saturn's and this toolkit's about it: the rows of the first
// page, the pad's buttons, the speeds, and the games' features handed over.
#pragma once

#include "../recomp-common/menu.h"
#include "../recomp-common/menu_layout.h"

#include <cstdint>
#include <string>

namespace port {

class Menu {
public:
    enum class Action { None, Save, Load, SetResetPoint, GoToResetPoint, ResetGame, ResetOptions, ExportPictures, ShowPictures, Quit };

    // withStates: show the save state rows and the reset point's
    void Open(bool withStates);
    void Close() {
        m_menu.Close();
    }
    bool IsOpen() const {
        return m_menu.IsOpen();
    }
    // Pad buttons newly pressed (port.h, kPad...): Up/Down choose, A/B/C/Start change or go into a page,
    // Left/Right change a row with several values.
    // Returns what the frontend is to do (with Slot()); it closes the menu if that went well.
    // (The tests' way in. A player's presses come through Press.)
    Action Buttons(uint16_t pressed);
    // The menu's own buttons newly pressed (recomp::kMenu... bits), which the frontend makes from keys and
    // gamepad buttons that are the same whatever the game's are set to (recomp-common/menu_sdl.h, MenuHeld, and the
    // game's own directions), held ones repeated (MenuRepeat)
    Action Press(unsigned buttons);
    // The mouse (recomp-common/menu_sdl.h, MouseEvent): what the menu returned for it
    Action ActionOf(const recomp::Menu::Chosen &chosen);
    // The shared menu itself (for the mouse)
    recomp::Menu &Shared() {
        return m_menu;
    }
    // The "Mute" row: the frontend plays nothing while this is set (--mute starts with it set)
    bool muted = false;
    int Slot() const {
        return m_menu.Slot();
    }
    void SetSlot(int slot) {
        m_menu.SetSlot(slot);
    }
    // How fast the game is to run: `frames` of the game's in `ticks` of the Saturn's field rate
    // (recomp-common/menu_layout.h, Speeds: 1/8 to 8x)
    recomp::layout::Speed Rate() const;
    bool FullSpeed() const {
        return Rate().frames == Rate().ticks;
    }
    // The speed's name ("Full", "1/2", "2x")
    std::string SpeedName() const {
        return Rate().name;
    }
    // A speed slower (by -1) or faster (+1) than now (the - and = keys); and full speed again
    void Faster(int by);
    void SetFullSpeed();
    // A line to tell the player (why not, say), until the next press
    void Note(const std::string &text) {
        m_menu.Note(text);
    }
    // Over a picture of 32-bit pixels (red in the low byte)
    void Draw(uint32_t *pixels, int width, int height) const {
        m_menu.Draw(pixels, width, height);
    }

private:
    // Tells the shared menu what this one has: its rows, the features, the speeds
    void Configure(bool withStates);
    // (the speeds alone, so that - and = work before the menu is first opened)
    void ConfigureSpeeds();

    recomp::Menu m_menu;
    bool m_configured = false;
};

} // namespace port
