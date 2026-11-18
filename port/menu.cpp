// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The in-game menu (menu.h): the shared menu told what a Saturn game's is made of.
#include "menu.h"

#include "game.h"
#include "pictures.h"
#include "port.h"

#include <algorithm>
#include <iterator>

namespace port {

namespace {
    // The first page's own rows
    enum { kSetResetPoint = 1, kGoToResetPoint, kResetGame };

    // The speeds, in hundredths
    const int kSpeeds[] = {100, 50, 25};
    const char *const kSpeedNames[] = {"100%", "50%", "25%"};
} // namespace

void Menu::Open(bool withStates) {
    Configure(withStates);
    m_menu.Open();
}

void Menu::Configure(bool withStates) {
    m_configured = true;
    recomp::MenuConfig config;
    // Groups with a line between them: the game's pages that head the menu (its Alternative modes), the save
    // states, what the game offers, and leaving
    std::vector<recomp::MenuRow> top;
    for (const satrecomp::Page &page : satrecomp::TheGame().pages) {
        // (pages that are ways of running the game: their rows do not count features that are on)
        config.pages.push_back({page.name, page.parent, page.description, false});
        if (!*page.parent) {
            top.push_back({recomp::MenuRow::Page, 0, page.name});
        }
    }
    if (!top.empty()) {
        config.groups.push_back(std::move(top));
    }
    if (withStates) {
        config.groups.push_back({{recomp::MenuRow::Save}, {recomp::MenuRow::Load}, {recomp::MenuRow::Slot}});
    }
    config.groups.push_back({{recomp::MenuRow::Pages}, {recomp::MenuRow::Speed}, {recomp::MenuRow::Mute}});
    if (withStates) {
        config.groups.back().push_back({recomp::MenuRow::Action, kSetResetPoint, "Set reset point",
                                        "Keeps the game as it is now as the point to practise from (F2)."});
        config.groups.back().push_back({recomp::MenuRow::Action, kGoToResetPoint, "Go to reset point",
                                        "Goes back to the reset point (F3, or the right stick's click)."});
    }
    if (satrecomp::TheGame().exportPictures) {
        // A Pictures page under Game (the page of the game's own options; on the first page for a game without one),
        // with its two commands (Press: the first exports, the second shows the folder)
        const bool gamePage = std::any_of(satrecomp::TheGame().features.begin(), satrecomp::TheGame().features.end(),
                                          [](const satrecomp::Feature &f) { return std::string(f.category) == "Game"; });
        const std::string folder = PicturesFolder();
        config.pages.push_back({"Pictures", gamePage ? "Game" : "",
                                "Export the game's pictures as PNG files, and show the folder they go to.", false});
        config.commands.push_back({"Pictures", "Export pictures",
                                   "Writes the game's pictures (its gallery and the like) as PNG files to " + folder +
                                       " (the pictures folder in your files: the launcher's Game page, YOUR FILES, Show). "
                                       "Files already there are written over."});
        config.commands.push_back({"Pictures", "Show pictures folder",
                                   "Opens " + folder + " (where Export pictures writes) in your file manager."});
    }
    config.groups.push_back({});
    if (withStates) {
        config.groups.back().push_back({recomp::MenuRow::Action, kResetGame, "Reset game",
                                        "Starts the game again, as when it was switched on. Your saves are kept."});
    }
    config.groups.back().push_back({recomp::MenuRow::Quit});
    config.groups.back().push_back({recomp::MenuRow::Resume});
    for (const satrecomp::Feature &feature : satrecomp::TheGame().features) {
        config.features.push_back({feature.category, feature.title, feature.description, {feature.choices.begin(), feature.choices.end()}});
    }
    // (what the player chose: a training aid shows as chosen outside a training session too, where it does nothing)
    config.value = [](size_t feature) { return satrecomp::FeatureSetting(satrecomp::TheGame().features[feature].name); };
    config.setValue = [](size_t feature, int value) { satrecomp::SetFeatureValue(satrecomp::TheGame().features[feature].name, value); };
    config.speeds.assign(std::begin(kSpeedNames), std::end(kSpeedNames));
    config.speedDescription = "Left/Right: how fast the game runs (- and = keys). F4 stops it and F8 goes on a frame at a time.";
    config.muted = [this] { return muted; };
    config.setMuted = [this](bool to) { muted = to; };
    config.confirmName = "Enter or A";
    config.closeName = "Esc";
    m_menu.Configure(std::move(config));
}

int Menu::Speed() const {
    // (the shared menu's place in the list, which is -1 until the menu is first told of the list: full speed)
    const int at = m_menu.Speed();
    return kSpeeds[at >= 0 && at < static_cast<int>(std::size(kSpeeds)) ? at : 0];
}

void Menu::SetSpeed(int speed) {
    // (the shared menu keeps the speed as a place in the list it was told of, and is only told when it is
    // first opened: the - and = keys must work before that too)
    if (!m_configured) {
        Configure(true);
    }
    m_menu.SetSpeed(speed >= 100 ? 0 : speed >= 50 ? 1 : 2);
}

Menu::Action Menu::Buttons(uint16_t pressed) {
    unsigned buttons = 0;
    buttons |= (pressed & kPadUp) ? recomp::kMenuUp : 0;
    buttons |= (pressed & kPadDown) ? recomp::kMenuDown : 0;
    buttons |= (pressed & kPadLeft) ? recomp::kMenuLeft : 0;
    buttons |= (pressed & kPadRight) ? recomp::kMenuRight : 0;
    buttons |= (pressed & (kPadA | kPadB | kPadC | kPadStart)) ? recomp::kMenuConfirm : 0;
    // (a press of something else still takes the note away, as it always did)
    if (pressed && !buttons && m_menu.IsOpen()) {
        m_menu.Note("");
    }
    return Press(buttons);
}

Menu::Action Menu::Press(unsigned buttons) {
    const recomp::Menu::Chosen chosen = m_menu.Buttons(buttons);
    switch (chosen.kind) {
    case recomp::Menu::Chosen::Save: return Action::Save;
    case recomp::Menu::Chosen::Load: return Action::Load;
    case recomp::Menu::Chosen::Quit: return Action::Quit;
    case recomp::Menu::Chosen::Command: return chosen.id == 0 ? Action::ExportPictures : Action::ShowPictures;
    case recomp::Menu::Chosen::Action:
        switch (chosen.id) {
        case kSetResetPoint: return Action::SetResetPoint;
        case kGoToResetPoint: return Action::GoToResetPoint;
        default: return Action::ResetGame;
        }
    default: return Action::None;
    }
}

} // namespace port
