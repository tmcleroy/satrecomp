// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// The in-game menu laid out the same way in every port: the pages' names, where each page is, the first page's
// groups, the speeds. A port fills MenuConfig's features, commands and its own pages, then calls Standard() for the
// rest; what a port has no rows for is left out by the menu itself (a page with nothing on it has no row).
//
// The first page, a line between each group:
//   Online > · Display > · Game > · (the port's head pages: Stage >, Replays >)
//   Alternative modes > (Training mode > and the other ways to play are under it) · (other pages the port puts here)
//   Save state · Load state · Slot
//   (the pages of other categories) · the port's own rows (Insert coin · Player 2 joins · Test menu)
//   Reset all options · Reset game · Quit game · Resume
// The Game page: Speed and Mute at its top, then Board >, Cheats >, Sound > and the game's own options.
// Training mode is under Alternative modes, Dummy under Training mode.
//
// Header only, C++17, no dependencies but menu.h.
#pragma once

#include "menu.h"

#include <algorithm>
#include <string>
#include <vector>

namespace recomp::layout {

// The pages' names: a port's features and commands take these as their category
constexpr const char *kOnline = "Online";
constexpr const char *kDisplay = "Display";
constexpr const char *kGame = "Game";
constexpr const char *kStage = "Stage";
constexpr const char *kBoard = "Board";
constexpr const char *kCheats = "Cheats";
constexpr const char *kSound = "Sound";
constexpr const char *kModes = "Alternative modes";
constexpr const char *kTraining = "Training mode";
constexpr const char *kDummy = "Dummy";

// The first page's own rows' words, the same in every port that has them
constexpr const char *kInsertCoin = "Insert coin";
constexpr const char *kPlayer2Joins = "Player 2 joins";
constexpr const char *kTestMenu = "Test menu";

struct Rows {
    // Pages after Online, Display and Game in the first group (Stage, Replays)
    std::vector<std::string> heads;
    // Pages in the second group after Alternative modes (the port's other ways of running the game)
    std::vector<std::string> modes;
    // Save state, Load state and Slot
    bool states = true;
    // The port's own rows after the other pages' (Insert coin, Player 2 joins, Test menu)
    std::vector<MenuRow> actions;
    // In place of the menu's own words for Reset game and Reset all options ("": the menu's)
    std::string resetDescription, resetOptionsDescription;
};

// Where a page is: under its parent, with a description when the port gave none; a page the port already has keeps
// its description and is moved under `parent`
inline void Place(MenuConfig &config, const std::string &name, const std::string &parent, const std::string &description = {},
                  std::vector<MenuRow::Kind> rows = {}) {
    for (MenuPage &page : config.pages) {
        if (page.name == name) {
            page.parent = parent;
            if (page.description.empty()) {
                page.description = description;
            }
            if (!rows.empty()) {
                page.rows = rows;
            }
            return;
        }
    }
    config.pages.push_back({name, parent, description, true, std::move(rows)});
}

// The standard layout: config.groups and where the standard pages are (MenuConfig::pages). Call after filling
// the features, commands and the port's own pages; config.groups is replaced.
inline void Standard(MenuConfig &config, const Rows &rows) {
    Place(config, kGame, "", "", {MenuRow::Speed, MenuRow::Mute});
    for (const char *under : {kBoard, kCheats, kSound}) {
        Place(config, under, kGame);
    }
    Place(config, kModes, "", "Other ways to play: training, and the game's other modes.");
    Place(config, kTraining, kModes, "A fight or a stage to practise in, and the aids for it.");
    Place(config, kDummy, kTraining, "What player 2 does in a training session.");
    config.groups.clear();
    std::vector<MenuRow> heads = {{MenuRow::Page, 0, kOnline}, {MenuRow::Page, 0, kDisplay}, {MenuRow::Page, 0, kGame}};
    for (const std::string &head : rows.heads) {
        heads.push_back({MenuRow::Page, 0, head});
    }
    config.groups.push_back(std::move(heads));
    std::vector<MenuRow> modes = {{MenuRow::Page, 0, kModes}};
    for (const std::string &page : rows.modes) {
        modes.push_back({MenuRow::Page, 0, page});
    }
    config.groups.push_back(std::move(modes));
    if (rows.states) {
        config.groups.push_back({{MenuRow::Save}, {MenuRow::Load}, {MenuRow::Slot}});
    }
    std::vector<MenuRow> others = {{MenuRow::Pages}};
    others.insert(others.end(), rows.actions.begin(), rows.actions.end());
    config.groups.push_back(std::move(others));
    config.groups.push_back({{MenuRow::ResetOptions, 0, "", rows.resetOptionsDescription},
                             {MenuRow::Reset, 0, "", rows.resetDescription},
                             {MenuRow::Quit},
                             {MenuRow::Resume}});
}

// The speeds, slowest first, each about 1.4 times the one before: the game runs `frames` frames in `ticks` of the
// screen's. Full speed is kFull; a port that cannot run faster than the board gives `faster` false (and has
// kSpeeds up to kFull alone).
struct Speed {
    const char *name;
    int frames, ticks;
};
constexpr Speed kSpeeds[] = {{"1/8", 1, 8}, {"1/6", 1, 6}, {"1/4", 1, 4}, {"1/3", 1, 3}, {"1/2", 1, 2}, {"3/4", 3, 4}, {"Full", 1, 1},
                             {"1.5x", 3, 2}, {"2x", 2, 1}, {"3x", 3, 1}, {"4x", 4, 1}, {"6x", 6, 1}, {"8x", 8, 1}};
constexpr int kFull = 6;
constexpr int kSpeedCount = static_cast<int>(sizeof kSpeeds / sizeof kSpeeds[0]);

inline void Speeds(MenuConfig &config, bool faster = true) {
    config.speeds.clear();
    for (int i = 0; i < (faster ? kSpeedCount : kFull + 1); i++) {
        config.speeds.push_back(kSpeeds[i].name);
    }
    config.normalSpeed = kFull;
}

// The speed chosen (Menu::Speed()) as frames in ticks
inline Speed SpeedOf(int chosen) {
    return kSpeeds[std::clamp(chosen, 0, kSpeedCount - 1)];
}

// The Speed row's description: how the speed is changed while playing (the standard keys)
constexpr const char *kSpeedDescription = "Left/Right: slow motion, or faster than the board. While playing, P stops the game and starts "
                                          "it again; O goes on by one frame; - and = change the speed.";

} // namespace recomp::layout
