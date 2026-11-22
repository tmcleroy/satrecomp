// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The in-game menu (menu.h): the shared menu told what a Saturn game's is made of.
#include "menu.h"

#include "game.h"
#include "pictures.h"
#include "port.h"

#include <string>

namespace port {

namespace {
    // The first page's own rows
    enum { kSetResetPoint = 1, kGoToResetPoint };
} // namespace

void Menu::Open(bool withStates) {
    Configure(withStates);
    m_menu.Open();
}

void Menu::Configure(bool withStates) {
    m_configured = true;
    namespace layout = recomp::layout;
    recomp::MenuConfig config;
    for (const satrecomp::Page &page : satrecomp::TheGame().pages) {
        // (pages that are ways of running the game: their rows do not count features that are on)
        config.pages.push_back({page.name, page.parent, page.description, false});
    }
    // The standard layout (recomp-common/menu_layout.h): Online, Display, Game; Alternative modes; the save
    // states; the other pages and the Saturn's own rows, the reset point's; Reset all options, Reset game, Quit
    // game, Resume. Speed and Mute are on the Game page, with Cheats and Sound under it.
    layout::Rows rows;
    rows.states = withStates;
    for (const satrecomp::Page &page : satrecomp::TheGame().pages) {
        // (the game's other pages at the head of the menu, after Alternative modes)
        if (!*page.parent && std::string(page.name) != layout::kModes) {
            rows.modes.push_back(page.name);
        }
    }
    if (withStates) {
        rows.actions.push_back({recomp::MenuRow::Action, kSetResetPoint, "Set reset point",
                                "Keeps the game as it is now as the point to practise from (F2)."});
        rows.actions.push_back({recomp::MenuRow::Action, kGoToResetPoint, "Go to reset point",
                                "Goes back to the reset point (F3, or the right stick's click)."});
    }
    rows.resetDescription = "Starts the game again, as when it was switched on. Your saves are kept.";
    for (const satrecomp::Feature &feature : satrecomp::TheGame().features) {
        config.features.push_back({feature.category, feature.title, feature.description, {feature.choices.begin(), feature.choices.end()}});
    }
    // (what the player chose: a training aid shows as chosen outside a training session too, where it does nothing)
    config.value = [](size_t feature) { return satrecomp::FeatureSetting(satrecomp::TheGame().features[feature].name); };
    config.setValue = [](size_t feature, int value) { satrecomp::SetFeatureValue(satrecomp::TheGame().features[feature].name, value); };
    layout::Speeds(config, true);
    config.speedDescription = "Left/Right: slow motion, or faster than the Saturn. While playing, P (or F4) stops the game and starts it "
                              "again; O (or F8) goes on by one frame; - and = change the speed.";
    config.muted = [this] { return muted; };
    config.setMuted = [this](bool to) { muted = to; };
    config.confirmName = "Enter or A";
    config.closeName = "Esc";
    layout::Standard(config, rows);
    if (satrecomp::TheGame().exportPictures) {
        // A Pictures page under Game (after Cheats and Sound), with its two commands (Press: the first exports, the
        // second shows the folder)
        const std::string folder = PicturesFolder();
        config.pages.push_back({"Pictures", layout::kGame, "Export the game's pictures as PNG files, and show the folder they go to.", false});
        config.commands.push_back({"Pictures", "Export pictures",
                                   "Writes the game's pictures (its gallery and the like) as PNG files to " + folder +
                                       " (the pictures folder in your files: the launcher's Game page, YOUR FILES, Show). "
                                       "Files already there are written over."});
        config.commands.push_back({"Pictures", "Show pictures folder",
                                   "Opens " + folder + " (where Export pictures writes) in your file manager."});
    }
    m_menu.Configure(std::move(config));
}

void Menu::ConfigureSpeeds() {
    // (the shared menu keeps the speed as a place in the list it was told of, and is otherwise only told when it is
    // first opened: the - and = keys must work before that too)
    if (!m_configured) {
        Configure(true);
    }
}

recomp::layout::Speed Menu::Rate() const {
    // (the shared menu's place in the list, which is -1 until the menu is first told of the list: full speed)
    const int at = m_menu.Speed();
    return recomp::layout::SpeedOf(at < 0 ? recomp::layout::kFull : at);
}

void Menu::Faster(int by) {
    ConfigureSpeeds();
    m_menu.SetSpeed(m_menu.Speed() + by);
}

void Menu::SetFullSpeed() {
    ConfigureSpeeds();
    m_menu.SetSpeed(recomp::layout::kFull);
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
    return ActionOf(m_menu.Buttons(buttons));
}

Menu::Action Menu::ActionOf(const recomp::Menu::Chosen &chosen) {
    switch (chosen.kind) {
    case recomp::Menu::Chosen::Save: return Action::Save;
    case recomp::Menu::Chosen::Load: return Action::Load;
    case recomp::Menu::Chosen::Quit: return Action::Quit;
    case recomp::Menu::Chosen::Reset: return Action::ResetGame;
    case recomp::Menu::Chosen::ResetOptions: return Action::ResetOptions;
    case recomp::Menu::Chosen::Command: return chosen.id == 0 ? Action::ExportPictures : Action::ShowPictures;
    case recomp::Menu::Chosen::Action: return chosen.id == kSetResetPoint ? Action::SetResetPoint : Action::GoToResetPoint;
    default: return Action::None;
    }
}

} // namespace port
