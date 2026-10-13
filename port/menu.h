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

#include <cstdint>
#include <string>

namespace port {

class Menu {
public:
    enum class Action { None, Save, Load, SetResetPoint, GoToResetPoint, Quit };

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
    Action Buttons(uint16_t pressed);
    int Slot() const {
        return m_menu.Slot();
    }
    void SetSlot(int slot) {
        m_menu.SetSlot(slot);
    }
    // How fast the game is to run, in hundredths (100, 50, 25)
    int Speed() const;
    void SetSpeed(int speed);
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

    recomp::Menu m_menu;
    bool m_configured = false;
};

} // namespace port
