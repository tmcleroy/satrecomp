// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// Which of a gamepad's buttons open the in-game menu: the player's choice on the launcher's Controllers page, kept in
// launcher.ini ([launcher] menu_button) and given to the games it starts in RECOMP_MENU_BUTTON, as the language is.
//
//   guide-stick   Guide, the left stick's click, or Start and Select together
//   guide         Guide, or Start and Select together (the left stick's click is the game's)
//   chord         Start and Select together only
//
// Start and Select together always open it: on a Steam Deck or a Steam Machine, Steam often takes Guide. The keyboard's
// keys (Esc, F1) are not this choice's. A port whose players should start with another of them builds both its game
// and its launcher with RECOMP_MENU_BUTTON_DEFAULT set to its key (the launcher's LAUNCHER_MENU_BUTTON does that for
// the launcher; launcher/launcher.cmake). No SDL here: menu_sdl.h asks it for each button.
#pragma once

#include <cstdlib>
#include <cstring>

#ifndef RECOMP_MENU_BUTTON_DEFAULT
#define RECOMP_MENU_BUTTON_DEFAULT "guide-stick"
#endif

namespace recomp {

enum class MenuButtons { GuideStick, Guide, Chord };

struct MenuButtonsChoice {
    MenuButtons value;
    const char *key;  // in launcher.ini and RECOMP_MENU_BUTTON
    const char *name; // as the launcher shows it
};

inline constexpr MenuButtonsChoice kMenuButtonsChoices[] = {
    {MenuButtons::GuideStick, "guide-stick", "Guide, left-stick click or Start+Select"},
    {MenuButtons::Guide, "guide", "Guide or Start+Select"},
    {MenuButtons::Chord, "chord", "Start+Select only"},
};

// The choice a key names, or `otherwise` (no key, or one not known)
inline MenuButtons ParseMenuButtons(const char *key, MenuButtons otherwise) {
    for (const MenuButtonsChoice &choice : kMenuButtonsChoices) {
        if (key && std::strcmp(key, choice.key) == 0) {
            return choice.value;
        }
    }
    return otherwise;
}

inline const char *MenuButtonsKey(MenuButtons buttons) {
    for (const MenuButtonsChoice &choice : kMenuButtonsChoices) {
        if (choice.value == buttons) {
            return choice.key;
        }
    }
    return kMenuButtonsChoices[0].key;
}

// The port's own (RECOMP_MENU_BUTTON_DEFAULT; recomp-common's: guide-stick)
inline MenuButtons DefaultMenuButtons() {
    return ParseMenuButtons(RECOMP_MENU_BUTTON_DEFAULT, MenuButtons::GuideStick);
}

// The game's: what the launcher said in RECOMP_MENU_BUTTON, or the port's own (a game started without it)
inline MenuButtons MenuButtonsFromEnvironment() {
    return ParseMenuButtons(std::getenv("RECOMP_MENU_BUTTON"), DefaultMenuButtons());
}

} // namespace recomp
