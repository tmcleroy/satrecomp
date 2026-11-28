// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// The in-game menu (menu.h).
#include "menu.h"

#include "i18n.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace recomp {

using namespace text;

namespace {
    // (after MenuRow's kinds, which the first page's rows keep)
    enum { kRowSave, kRowLoad, kRowSlot, kRowSpeed, kRowAction, kRowQuit, kRowResume, kRowCommand, kRowPage, kRowFeature, kRowBack, kRowMute, kRowFavourites, kRowReset, kRowResetOptions, kRowNo, kRowYes };
    constexpr unsigned kArrows = kMenuUp | kMenuDown | kMenuLeft | kMenuRight;
    enum { kValuePlain, kValueDim, kValueOn }; // (how a row's value is shown: Menu::Describe)
} // namespace

unsigned MenuRepeat::Step(unsigned held) {
    unsigned pressed = held & ~m_held;
    // (the arrows held, and nothing else gone down: on with the count; otherwise from the start)
    const unsigned arrows = held & kArrows;
    if (arrows && arrows == (m_held & kArrows) && !pressed) {
        m_frames++;
        const bool faster = m_frames >= kFasterAfter;
        const int after = faster ? kFasterAfter : kRepeatAfter, every = faster ? kFasterEvery : kRepeatEvery;
        if (m_frames >= kRepeatAfter && (m_frames - after) % every == 0) {
            pressed |= arrows;
        }
    } else {
        m_frames = 0;
    }
    m_held = held;
    return pressed;
}

bool Menu::Favourites(size_t feature) const {
    return m_config.features[feature].favourites && !m_config.features[feature].choices.empty() && m_config.favourite &&
           m_config.setFavourite;
}

void Menu::Configure(MenuConfig config) {
    m_config = std::move(config);
    m_speed = std::clamp(m_speed < 0 ? m_config.normalSpeed : m_speed, 0, std::max(static_cast<int>(m_config.speeds.size()) - 1, 0));
    m_slot = std::min(m_slot, SlotCount());
    if (m_open) {
        Fill();
    }
}

void Menu::SetSpeed(int speed) {
    m_speed = std::clamp(speed, 0, std::max(static_cast<int>(m_config.speeds.size()) - 1, 0));
}

const MenuPage *Menu::PageOf(const std::string &name) const {
    for (const MenuPage &page : m_config.pages) {
        if (page.name == name) {
            return &page;
        }
    }
    return nullptr;
}

std::string Menu::Parent(const std::string &page) const {
    const MenuPage *of = PageOf(page);
    return of ? of->parent : std::string();
}

// A page is there when something is on it: a feature, a command, or a page under it that is there
bool Menu::Exists(const std::string &page) const {
    if (page.empty()) {
        return true;
    }
    if (std::any_of(m_config.features.begin(), m_config.features.end(), [&](const MenuFeature &f) { return f.category == page; }) ||
        std::any_of(m_config.commands.begin(), m_config.commands.end(), [&](const MenuCommand &c) { return c.category == page; }) ||
        std::any_of(m_config.pages.begin(), m_config.pages.end(), [&](const MenuPage &p) { return p.name == page && !p.rows.empty(); })) {
        return true;
    }
    // (or a page under it has: looked at one level down, which is what a page of pages needs, and cannot go
    // round for ever whatever the host has named as whose parent)
    for (const MenuPage &under : m_config.pages) {
        if (under.parent == page && under.name != page &&
            (std::any_of(m_config.features.begin(), m_config.features.end(), [&](const MenuFeature &f) { return f.category == under.name; }) ||
             std::any_of(m_config.commands.begin(), m_config.commands.end(), [&](const MenuCommand &c) { return c.category == under.name; }))) {
            return true;
        }
    }
    return false;
}

// Back to the page this one was reached from, on the row it was left on
void Menu::Leave() {
    m_page = Parent(m_page);
    if (Tabbed() && InTab(m_page)) {
        m_page.clear(); // (the tab itself)
    }
    m_selected = m_left.empty() ? 0 : m_left.back();
    if (!m_left.empty()) {
        m_left.pop_back();
    }
    if (m_page.empty()) {
        m_left.clear();
    }
    Fill();
}

// Into a page from the one shown, at its top
void Menu::Enter(const std::string &page) {
    m_left.push_back(m_selected);
    m_page = page;
    m_selected = 0;
    Fill();
}

// A page's rows: its own Speed and Mute rows, its commands, the pages under it and its features (tabbed: the pages under
// it last, but for those that are a tab's own)
void Menu::PageRows(const std::string &name, bool tabbed) {
    for (const MenuPage &page : m_config.pages) {
        for (const MenuRow::Kind kind : page.name == name ? page.rows : std::vector<MenuRow::Kind>()) {
            if (kind == MenuRow::Speed && !m_config.speeds.empty()) {
                m_rows.push_back({kRowSpeed, 0, ""});
            } else if (kind == MenuRow::Mute && m_config.muted && m_config.setMuted) {
                m_rows.push_back({kRowMute, 0, ""});
            }
        }
    }
    for (size_t i = 0; i < m_config.commands.size(); i++) {
        if (name == m_config.commands[i].category) {
            m_rows.push_back({kRowCommand, i, ""});
        }
    }
    const auto pages = [&] {
        for (const MenuPage &under : m_config.pages) {
            if (under.parent == name && under.name != name && Exists(under.name) && !(tabbed && InTab(under.name))) {
                m_rows.push_back({kRowPage, 0, under.name});
            }
        }
    };
    if (!tabbed) {
        pages();
    }
    for (size_t i = 0; i < m_config.features.size(); i++) {
        if (name == m_config.features[i].category) {
            m_rows.push_back({kRowFeature, i, ""});
            if (Favourites(i)) {
                m_rows.push_back({kRowFavourites, i, ""});
            }
        }
    }
    if (tabbed) {
        pages(); // (in a tab, after the page's own options)
    }
}

// The rows of the page being shown
void Menu::Fill() {
    if (Tabbed()) {
        FillTabbed();
        return;
    }
    m_rows.clear();
    m_first.clear();
    // (Reset all options asked about: No and Yes alone; the first page's rows are kept, for the row's description)
    if (m_confirm) {
        for (const std::vector<MenuRow> &group : m_config.groups) {
            m_first.insert(m_first.end(), group.begin(), group.end());
        }
        if (m_confirmIndex < m_first.size() && m_first[m_confirmIndex].kind == MenuRow::ResetOptions) {
            m_rows.push_back({kRowNo, m_confirmIndex, ""});
            m_rows.push_back({kRowYes, m_confirmIndex, ""});
            m_selected = std::clamp(m_selected, 0, 1);
            return;
        }
        // (the row is no longer there: back to the page)
        m_first.clear();
        m_confirm = false;
        m_selected = m_confirmFrom;
    }
    // (a page that is no longer there: the first page)
    if (!m_page.empty() && !Exists(m_page)) {
        m_page.clear();
        m_selected = m_left.empty() ? 0 : m_left.front();
        m_left.clear();
    }
    if (m_page.empty()) {
        for (const std::vector<MenuRow> &group : m_config.groups) {
            bool divider = !m_rows.empty();
            auto add = [&](Row row) {
                row.divider = divider;
                divider = false;
                m_rows.push_back(std::move(row));
            };
            for (const MenuRow &row : group) {
                const size_t index = m_first.size();
                m_first.push_back(row);
                switch (row.kind) {
                case MenuRow::Save: add({kRowSave, index, ""}); break;
                case MenuRow::Load: add({kRowLoad, index, ""}); break;
                case MenuRow::Slot: add({kRowSlot, index, ""}); break;
                case MenuRow::Speed:
                    if (!m_config.speeds.empty()) {
                        add({kRowSpeed, index, ""});
                    }
                    break;
                case MenuRow::Action: add({kRowAction, index, ""}); break;
                case MenuRow::Quit: add({kRowQuit, index, ""}); break;
                case MenuRow::Resume: add({kRowResume, index, ""}); break;
                case MenuRow::Reset: add({kRowReset, index, ""}); break;
                case MenuRow::ResetOptions: add({kRowResetOptions, index, ""}); break;
                case MenuRow::Mute:
                    if (m_config.muted && m_config.setMuted) {
                        add({kRowMute, index, ""});
                    }
                    break;
                case MenuRow::Page:
                    if (!row.title.empty() && Exists(row.title)) {
                        add({kRowPage, index, row.title});
                    }
                    break;
                case MenuRow::Pages: {
                    for (size_t i = 0; i < m_config.commands.size(); i++) {
                        if (m_config.commands[i].category.empty()) {
                            add({kRowCommand, i, ""});
                        }
                    }
                    // A page for each category, in the order they first come up: but for the pages that are
                    // under another page, and those the host has given a row of their own
                    std::vector<std::string> pages;
                    for (const std::vector<MenuRow> &others : m_config.groups) {
                        for (const MenuRow &other : others) {
                            if (other.kind == MenuRow::Page) {
                                pages.push_back(other.title);
                            }
                        }
                    }
                    auto addPage = [&](const std::string &category) {
                        if (!category.empty() && std::find(pages.begin(), pages.end(), category) == pages.end()) {
                            pages.push_back(category);
                            if (Parent(category).empty()) {
                                add({kRowPage, 0, category});
                            }
                        }
                    };
                    for (const MenuFeature &feature : m_config.features) {
                        addPage(feature.category);
                    }
                    for (const MenuCommand &command : m_config.commands) {
                        addPage(command.category);
                    }
                    break;
                }
                }
            }
        }
    } else {
        PageRows(m_page, false);
        m_rows.push_back({kRowBack, 0, ""});
    }
    m_selected = std::clamp(m_selected, 0, std::max(static_cast<int>(m_rows.size()) - 1, 0));
}

// From the page of No and Yes back to the page Reset all options is on, on its row
void Menu::EndConfirm() {
    m_confirm = false;
    m_selected = m_confirmFrom;
    Fill();
}

void Menu::Open() {
    m_note.clear();
    m_confirm = false; // (asked again each time: never left open across a close)
    Fill(); // (on the page, and the row, it was closed on)
    m_open = true;
}

Menu::Chosen Menu::Buttons(unsigned pressed) {
    if (Tabbed()) {
        return ButtonsTabbed(pressed);
    }
    if (!m_open || !pressed || m_rows.empty()) {
        return {};
    }
    m_note.clear();
    if (m_confirm && (pressed & (kMenuBack | kMenuLeft))) {
        EndConfirm(); // (Back and Left: No)
        return {};
    }
    if (pressed & kMenuBack) {
        if (m_page.empty()) {
            m_open = false;
        } else {
            Leave();
        }
        return {};
    }
    const int count = static_cast<int>(m_rows.size());
    if (pressed & kMenuUp) {
        m_selected = (m_selected + count - 1) % count;
    }
    if (pressed & kMenuDown) {
        m_selected = (m_selected + 1) % count;
    }
    return RowButtons(pressed);
}

// What a press does on the row chosen (the arrows Up and Down already done)
Menu::Chosen Menu::RowButtons(unsigned pressed) {
    if (m_rows.empty()) {
        return {};
    }
    const Row row = m_rows[static_cast<size_t>(std::clamp(m_selected, 0, static_cast<int>(m_rows.size()) - 1))];
    const bool press = pressed & kMenuConfirm;
    // Rows with several values: Left and Right go through them (Confirm goes on to the next, around)
    const int step = (pressed & kMenuLeft) ? -1 : (press || (pressed & kMenuRight)) ? 1 : 0;
    if (row.kind == kRowSlot && step) { // 1 to SlotCount(), around
        SetSlot(m_slot + step);
        return {};
    }
    if (row.kind == kRowMute && step) {
        m_config.setMuted(!m_config.muted());
        return {};
    }
    if (row.kind == kRowSpeed && step) {
        const int speeds = static_cast<int>(m_config.speeds.size());
        m_speed = press ? (m_speed + 1) % speeds : std::clamp(m_speed + step, 0, speeds - 1);
        return {};
    }
    if (row.kind == kRowFeature && press && Favourites(row.index)) {
        // (Confirm on a favourites feature: the choice shown marked a favourite, or no longer; a list of stages may have
        // no choice that means off, its first a stage too)
        const int value = m_config.value ? m_config.value(row.index) : 0;
        m_config.setFavourite(row.index, value, !m_config.favourite(row.index, value));
        return {};
    }
    if (row.kind == kRowFavourites && step) {
        // (the next favourite that way, around, from the choice shown; none marked: as it is)
        const int choices = static_cast<int>(m_config.features[row.index].choices.size());
        const int value = m_config.value ? m_config.value(row.index) : 0;
        for (int n = 1; n < choices && m_config.setValue; n++) {
            const int at = ((value + step * n) % choices + choices) % choices;
            if (m_config.favourite(row.index, at)) {
                m_config.setValue(row.index, at);
                break;
            }
        }
        return {};
    }
    if (row.kind == kRowFeature && step && (press || !m_config.features[row.index].choices.empty())) {
        const MenuFeature &feature = m_config.features[row.index];
        const int value = m_config.value ? m_config.value(row.index) : 0;
        if (!m_config.setValue) {
            return {};
        }
        if (feature.choices.empty()) {
            if (press) {
                m_config.setValue(row.index, value ? 0 : 1);
            }
        } else {
            const int choices = static_cast<int>(feature.choices.size());
            // (a value past the list's end, of a list built at run time that has become shorter, as its last: Left
            // goes back into it)
            const int now = std::clamp(value, 0, choices - 1);
            auto offered = [&](int choice) { return choice == 0 || choice == now || !m_config.offered || m_config.offered(row.index, choice); };
            int to = now;
            if (press) {
                // (around, to the next one offered: off at the latest)
                do {
                    to = (to + 1) % choices;
                } while (!offered(to));
            } else {
                // (to the next one offered that way; none: as it is)
                for (int at = now + step; at >= 0 && at < choices; at += step) {
                    if (offered(at)) {
                        to = at;
                        break;
                    }
                }
            }
            m_config.setValue(row.index, to);
        }
        return {};
    }
    if (!press) {
        // (Left on a page's other rows: back to the page it was reached from; Right on a page's row: into it)
        if ((pressed & kMenuLeft) && !m_page.empty()) {
            Leave();
        } else if ((pressed & kMenuRight) && row.kind == kRowPage) {
            Enter(row.page);
        }
        return {};
    }
    switch (row.kind) {
    case kRowSave: return {Chosen::Save, 0};
    case kRowLoad: return {Chosen::Load, 0};
    case kRowAction: return {Chosen::Action, m_first[row.index].id};
    case kRowQuit: return {Chosen::Quit, 0};
    case kRowReset: return {Chosen::Reset, 0};
    case kRowResetOptions:
        // (asked first: a booby trap otherwise, next to Reset game)
        m_confirm = true;
        m_confirmIndex = row.index;
        m_confirmFrom = m_selected;
        m_selected = 0; // (No)
        Fill();
        break;
    case kRowNo: EndConfirm(); break;
    case kRowYes:
        EndConfirm();
        SetSpeed(m_config.normalSpeed);
        return {Chosen::ResetOptions, 0};
    case kRowResume: m_open = false; break;
    case kRowCommand: return {Chosen::Command, static_cast<int>(row.index)};
    case kRowPage: Enter(row.page); break;
    case kRowBack: Leave(); break;
    default: break;
    }
    return {};
}

Menu::Chosen Menu::Mouse(MenuMouse what, float x, float y) {
    if (Tabbed()) {
        return MouseTabbed(what, x, y);
    }
    if (!m_open || m_rows.empty()) {
        return {};
    }
    switch (what) {
    case MenuMouse::Back: return Buttons(kMenuBack);
    case MenuMouse::WheelUp: return Buttons(kMenuUp);
    case MenuMouse::WheelDown: return Buttons(kMenuDown);
    default: break;
    }
    // (the row under the mouse, as last drawn; none: away from the rows)
    int over = -1;
    for (const Shown &shown : m_shown) {
        if (x >= m_panelLeft && x < m_panelRight && y >= shown.top && y < shown.bottom && shown.row < static_cast<int>(m_rows.size())) {
            over = shown.row;
        }
    }
    if (over < 0) {
        return {};
    }
    m_selected = over;
    return what == MenuMouse::Click ? Buttons(kMenuConfirm) : Chosen{};
}

// A row's words: its title, its value (and how the value is shown: kValue...) and its description
void Menu::Describe(const Row &row, std::string &title, std::string &value, int &valueColor, std::string &rowDescription) const {
    title = Tr("Resume");
    rowDescription = Tr("Back to the game.");
    switch (row.kind) {
    case kRowSave:
        title = Tr("Save state");
        rowDescription = Tr("Keeps the game as it is now in the slot.");
        break;
    case kRowLoad:
        title = Tr("Load state");
        rowDescription = Tr("Goes back to the game kept in the slot.");
        break;
    case kRowSlot:
        title = Tr("Slot");
        value = std::to_string(m_slot);
        rowDescription = i18n::TrFormat("Left/Right: the slot (1-{}) that Save state and Load state use.", {std::to_string(SlotCount())});
        break;
    case kRowSpeed:
        title = Tr("Speed");
        value = Tr(m_config.speeds[static_cast<size_t>(m_speed)]);
        valueColor = m_speed == m_config.normalSpeed ? kValueDim : kValueOn;
        rowDescription = Tr(m_config.speedDescription);
        break;
    case kRowMute:
        title = Tr("Mute");
        value = Tr(m_config.muted() ? "On" : "Off");
        valueColor = m_config.muted() ? kValueOn : kValueDim;
        rowDescription = Tr("Switches the game's sound off, and on again.");
        break;
    case kRowAction:
        title = Tr(m_first[row.index].title);
        rowDescription = Tr(m_first[row.index].description);
        break;
    case kRowReset:
        title = Tr("Reset game");
        rowDescription = Tr(m_first[row.index].description.empty() ? "Starts the game again, as if switched off and on."
                                                                    : m_first[row.index].description.c_str());
        break;
    case kRowResetOptions:
        title = Tr("Reset all options");
        rowDescription = Tr(m_first[row.index].description.empty() ? "Puts every option back as it first was, and the speed to full."
                                                                    : m_first[row.index].description.c_str());
        break;
    case kRowNo:
        title = Tr("No");
        rowDescription = Tr("Keeps every option as it is.");
        break;
    case kRowYes:
        title = Tr("Yes");
        rowDescription = Tr(m_first[row.index].description.empty() ? "Puts every option back as it first was, and the speed to full."
                                                                    : m_first[row.index].description.c_str());
        break;
    case kRowQuit:
        title = Tr("Quit game");
        rowDescription = Tr("Closes the game.");
        break;
    case kRowCommand: {
        const MenuCommand &command = m_config.commands[row.index];
        title = Tr(command.title);
        rowDescription = Tr(command.description);
        break;
    }
    case kRowPage: {
        title = Tr(row.page);
        value = ">";
        valueColor = kValueDim;
        // (what is on the page, and how much of it is on, unless the page is one that does not say)
        const MenuPage *page = PageOf(row.page);
        int onCount = 0;
        for (size_t f = 0; f < m_config.features.size() && (!page || page->counts); f++) {
            if (row.page == m_config.features[f].category) {
                onCount += m_config.value && m_config.value(f) != 0;
            }
        }
        if (onCount) {
            value = i18n::TrFormat("{} on  >", {std::to_string(onCount)});
            valueColor = kValueOn;
        }
        rowDescription = page && !page->description.empty() ? Tr(page->description)
                                                             : i18n::TrFormat("The page of {} choices.", {Tr(row.page)});
        break;
    }
    case kRowBack:
        title = Tr("Back");
        rowDescription = Parent(m_page).empty() ? Tr("To the first page (Left does the same).")
                                                : i18n::TrFormat("To the {} page (Left does the same).", {Tr(Parent(m_page))});
        break;
    case kRowFeature: {
        const MenuFeature &feature = m_config.features[row.index];
        const int chosen = m_config.value ? m_config.value(row.index) : 0;
        title = Tr(feature.title);
        rowDescription = Tr(feature.description);
        if (feature.choices.empty()) {
            value = Tr(chosen ? "On" : "Off");
        } else {
            value = Tr(feature.choices[static_cast<size_t>(std::clamp(chosen, 0, static_cast<int>(feature.choices.size()) - 1))]);
        }
        valueColor = chosen ? kValueOn : kValueDim;
        if (Favourites(row.index)) {
            if (m_config.favourite(row.index, chosen)) {
                value += "  *";
            }
            rowDescription += i18n::TrFormat(" ({}: the one shown a favourite, or not.)", {Tr(m_config.confirmName)});
        }
        break;
    }
    case kRowFavourites: {
        const MenuFeature &feature = m_config.features[row.index];
        const int chosen = m_config.value ? m_config.value(row.index) : 0;
        title = Tr("Favourites"); // (of the row above's)
        int count = 0;
        for (int c = 0; c < static_cast<int>(feature.choices.size()); c++) {
            count += m_config.favourite(row.index, c) ? 1 : 0;
        }
        if (!count) {
            value = Tr("none yet");
            valueColor = kValueDim;
        } else if (chosen >= 0 && chosen < static_cast<int>(feature.choices.size()) && m_config.favourite(row.index, chosen)) {
            value = Tr(feature.choices[static_cast<size_t>(chosen)]);
            valueColor = kValueOn;
        } else {
            value = std::to_string(count) + " >";
            valueColor = kValueDim;
        }
        rowDescription = i18n::TrFormat("Left/Right: through your favourites of {} alone. {} on the row above marks the one shown a "
                                        "favourite, or not.",
                                        {Tr(feature.title), Tr(m_config.confirmName)});
        break;
    }
    default: break;
    }
}

void Menu::Draw(uint32_t *pixels, int width, int height) const {
    if (Tabbed()) {
        DrawTabbed(pixels, width, height);
        return;
    }
    static constexpr Color white{255, 255, 255, 255}, dim{160, 164, 176, 255}, on{110, 220, 120, 255};
    m_shown.clear();
    if (m_rows.empty()) {
        return;
    }
    const Canvas canvas{pixels, width, height};
    // Sizes in font dots: d for the small text, 2d for the rows; as the window's height allows, and no larger than lets
    // the widest row fit across (a tall, narrow window: a game's picture stood up). Set below, after the rows' words
    float d = 1, pad = 0, panelWidth = 0, inner = 0;
    // (the rows' letters, in dots of d: twice the small ones, or the small ones themselves when compact)
    const float rowLetters = m_compact ? 1.0f : 2.0f;
    {
        // (the widest row at a dot of 1: its title in the rows' letters, the gap, and its value in them or at least in the
        // small letters, cut to a few characters if it must be: Draw cuts a long value so)
        float widest = 0;
        for (const Row &row : m_rows) {
            std::string title, value, rowDescription;
            int valueColor = kValuePlain;
            Describe(row, title, value, valueColor, rowDescription);
            float need = TextWidth(title.c_str(), rowLetters);
            if (!value.empty()) {
                need += 6 + std::min(TextWidth(value.c_str(), rowLetters), std::min(TextWidth(value.c_str(), 1), TextWidth("......", 1)));
            }
            widest = std::max(widest, need);
        }
        const float byHeight = static_cast<float>(std::max(height / 300, 1));
        const float byWidth = std::floor(width * 0.9f / (widest + 16)); // (16: the panel's padding, 8 dots each side)
        d = std::max(1.0f, std::min(byHeight, byWidth));
        pad = 8 * d;
        panelWidth = std::min(150 * kCharWidth * d / 2, width * 0.9f); // 75 small characters, if there is room
        inner = panelWidth - 2 * pad;
    }
    // (the hint is one line unless the picture is a narrow one)
    const std::string hint =
        i18n::TrFormat("Up/Down: choose   Left/Right or {}: change   {}: close", {Tr(m_config.confirmName), Tr(m_config.closeName)});
    const std::string note = Tr(m_note);
    const int hintLines = TextLines(hint.c_str(), d, inner);
    // (rows closer together when there are many; and when even that does not fit, the rows about the chosen
    // one are shown)
    // (room for the longest description on the page, three lines at least, so that it never runs into the hint)
    int descriptionLines = 3;
    for (const Row &row : m_rows) {
        std::string title, value, rowDescription;
        int valueColor = kValuePlain;
        Describe(row, title, value, valueColor, rowDescription);
        descriptionLines = std::max(descriptionLines, TextLines(rowDescription.c_str(), d, inner));
    }
    descriptionLines = std::max(descriptionLines, TextLines(note.c_str(), d, inner));
    const float kDivider = m_compact ? 5.0f : 9.0f; // (the room a line between groups takes)
    // (a row's height, in dots: room for the letters with what reaches below the line (g, p, y) and a gap, up to a
    // little more when there are few rows)
    const float rowLeast = m_compact ? 10.0f : 15.0f, rowMost = m_compact ? 12.0f : 22.0f;
    const float dividers = static_cast<float>(std::count_if(m_rows.begin(), m_rows.end(), [](const Row &row) { return row.divider; }));
    // (compact: the rows first, eight of them at least where there are so many, and the description in what is left,
    // cut short if it needs more: a long one would otherwise leave a screen of few lines a row or two)
    if (m_compact) {
        const float rowsRoom = static_cast<float>(std::min<size_t>(m_rows.size(), 8)) * rowLeast + dividers * kDivider;
        const float left = height / d - (8 + 16 + 6 + 8 + 4 + hintLines * kLineHeight + 8) - rowsRoom;
        descriptionLines = std::min(descriptionLines, std::max(2, static_cast<int>(left / kLineHeight)));
    }
    const float around = 8 + 16 + 6 + 8 + descriptionLines * kLineHeight + 4 + hintLines * kLineHeight + 8;
    const float room = height / d - around - dividers * kDivider;
    const size_t shown = std::min(m_rows.size(), static_cast<size_t>(std::max(room / rowLeast, 1.0f)));
    const float rowHeight = std::clamp(room / shown, rowLeast, rowMost) * d;
    const size_t first = std::min(static_cast<size_t>(std::max(m_selected - static_cast<int>(shown) / 2, 0)), m_rows.size() - shown);
    const float panelHeight = pad + 16 * d + 6 * d + shown * rowHeight + dividers * kDivider * d + 8 * d + descriptionLines * kLineHeight * d + 4 * d + hintLines * kLineHeight * d + pad;
    const float px = (width - panelWidth) / 2, py = (height - panelHeight) / 2;

    canvas.Fill(0, 0, static_cast<float>(width), static_cast<float>(height), {0, 0, 0, 150});
    canvas.Fill(px, py, panelWidth, panelHeight, {20, 22, 30, 255}); // (opaque: the game's own text would show through)

    const float x = px + pad;
    float y = py + pad;
    m_panelLeft = px;
    m_panelRight = px + panelWidth;
    {
        // (the page's name; one too long for the panel, a narrow or low screen: in the small letters)
        const std::string heading = m_confirm ? Tr("Reset all options: are you sure?") : Tr(m_page.empty() ? "Menu" : m_page.c_str());
        const bool fits = TextWidth(heading.c_str(), 2 * d) <= inner;
        canvas.Text(x, fits ? y : y + 4 * d, fits ? 2 * d : d, 0, white, heading.c_str());
    }
    if (shown < m_rows.size()) {
        const std::string place = i18n::TrFormat("{} of {}", {std::to_string(m_selected + 1), std::to_string(m_rows.size())});
        canvas.Text(x + inner - TextWidth(place.c_str(), d), y + 4 * d, d, 0, dim, place.c_str());
    }
    y += 16 * d + 6 * d;
    std::string description;
    for (size_t i = first; i < first + shown; i++, y += rowHeight) {
        const bool selected = static_cast<int>(i) == m_selected;
        if (m_rows[i].divider && i > first) {
            canvas.Fill(x, y + (kDivider * d - d) / 2, inner, d, {255, 255, 255, 70});
            y += kDivider * d;
        }
        if (selected) {
            canvas.Fill(px + pad / 2, y, panelWidth - pad, rowHeight, {255, 255, 255, 40});
        }
        m_shown.push_back({static_cast<int>(i), y, y + rowHeight});
        std::string title, value, rowDescription;
        int valueKind = kValuePlain;
        Describe(m_rows[i], title, value, valueKind, rowDescription);
        const Color valueColor = valueKind == kValueOn ? on : valueKind == kValueDim ? dim : white;
        const float big = rowLetters * d; // (the rows' dot)
        // (the letters' 7 dots in the middle of the row; compact, their 8 with what reaches below the line)
        const float ty = y + (rowHeight - (m_compact ? 8 : 7) * big) / 2;
        // (a title too long for the panel even at the smallest dot, the window very narrow: in the small letters)
        const float titleDot = TextWidth(title.c_str(), big) <= inner ? big : d;
        canvas.Text(x, titleDot < big ? ty + 4 * d : ty, titleDot, 0, white, title.c_str());
        if (!value.empty()) {
            // (a value too long for the room beside the title: in the small letters, and cut short if even
            // those do not fit)
            const float room = inner - TextWidth(title.c_str(), titleDot) - 6 * d;
            if (TextWidth(value.c_str(), big) <= room) {
                canvas.Text(x + inner - TextWidth(value.c_str(), big), ty, big, 0, valueColor, value.c_str());
            } else {
                std::string small = value;
                if (TextWidth(small.c_str(), d) > room) {
                    while (small.size() > 1 && TextWidth((small + "..").c_str(), d) > room) {
                        small.pop_back();
                        while (small.size() > 1 && (static_cast<unsigned char>(small.back()) & 0xC0) == 0x80) {
                            small.pop_back(); // (the whole of a character of several bytes)
                        }
                    }
                    small += "..";
                }
                canvas.Text(x + inner - TextWidth(small.c_str(), d), big > d ? ty + 4 * d : ty, d, 0, valueColor, small.c_str());
            }
        }
        if (selected) {
            description = rowDescription;
        }
    }
    y += 8 * d;
    // (cut to its lines, with ".." at the end, where compact left it fewer than it needs)
    std::string said = note.empty() ? description : note;
    if (TextLines(said.c_str(), d, inner) > descriptionLines) {
        while (said.size() > 1 && TextLines((said + "..").c_str(), d, inner) > descriptionLines) {
            said.pop_back();
            while (said.size() > 1 && (static_cast<unsigned char>(said.back()) & 0xC0) == 0x80) {
                said.pop_back(); // (the whole of a character of several bytes)
            }
        }
        said += "..";
    }
    canvas.Text(x, y, d, inner, note.empty() ? dim : white, said.c_str());
    y += descriptionLines * kLineHeight * d + 4 * d;
    canvas.Text(x, y, d, inner, dim, hint.c_str());
}

// The tabbed menu (MenuConfig::tabs): a row of commands, the tabs, the tab's rows. Its looks share everything but how
// they are drawn: what is on it and where, the buttons and the mouse.

// A page that is one of a tab's (and so not a row of its parent's)
bool Menu::InTab(const std::string &page) const {
    for (const MenuTab &tab : m_config.tabs) {
        if (std::find(tab.pages.begin(), tab.pages.end(), page) != tab.pages.end()) {
            return true;
        }
    }
    return false;
}

// A tab's page with rows of its own there: a feature, a command, a Speed or Mute row, or a page under it that is not a
// tab's
bool Menu::TabPageShown(const std::string &name) const {
    if (std::any_of(m_config.features.begin(), m_config.features.end(), [&](const MenuFeature &f) { return f.category == name; }) ||
        std::any_of(m_config.commands.begin(), m_config.commands.end(), [&](const MenuCommand &c) { return c.category == name; })) {
        return true;
    }
    for (const MenuPage &page : m_config.pages) {
        if (page.name == name && !page.rows.empty() &&
            std::any_of(page.rows.begin(), page.rows.end(), [&](MenuRow::Kind kind) {
                return (kind == MenuRow::Speed && !m_config.speeds.empty()) || (kind == MenuRow::Mute && m_config.muted && m_config.setMuted);
            })) {
            return true;
        }
        if (page.parent == name && page.name != name && !InTab(page.name) && Exists(page.name)) {
            return true;
        }
    }
    return false;
}

int Menu::FeatureDefault(size_t feature) const {
    return m_config.defaultValue ? m_config.defaultValue(feature) : 0;
}

bool Menu::Changed(size_t feature) const {
    return m_config.value && m_config.value(feature) != FeatureDefault(feature);
}

void Menu::FillTabbed() {
    m_rows.clear();
    m_first.clear();
    m_bar.clear();
    // The command row: Back, the host's, Resume
    m_bar.push_back({kRowBack, 0});
    for (const MenuRow &row : m_config.bar) {
        const size_t index = m_first.size();
        m_first.push_back(row);
        switch (row.kind) {
        case MenuRow::Save: m_bar.push_back({kRowSave, index}); break;
        case MenuRow::Load: m_bar.push_back({kRowLoad, index}); break;
        case MenuRow::Slot: m_bar.push_back({kRowSlot, index}); break;
        case MenuRow::Reset: m_bar.push_back({kRowReset, index}); break;
        case MenuRow::Quit: m_bar.push_back({kRowQuit, index}); break;
        case MenuRow::Action: m_bar.push_back({kRowAction, index}); break;
        case MenuRow::Mute:
            if (m_config.muted && m_config.setMuted) {
                m_bar.push_back({kRowMute, index});
            }
            break;
        case MenuRow::Speed:
            if (!m_config.speeds.empty()) {
                m_bar.push_back({kRowSpeed, index});
            }
            break;
        default: break;
        }
    }
    m_bar.push_back({kRowResume, 0});
    m_barAt = std::clamp(m_barAt, 0, static_cast<int>(m_bar.size()) - 1);

    // The tabs with something on them
    const auto rowShown = [&](const MenuRow &row) {
        switch (row.kind) {
        case MenuRow::Speed: return !m_config.speeds.empty();
        case MenuRow::Mute: return m_config.muted && m_config.setMuted;
        case MenuRow::Page: return !row.title.empty() && Exists(row.title);
        case MenuRow::Pages:
            return std::any_of(m_config.commands.begin(), m_config.commands.end(), [](const MenuCommand &c) { return c.category.empty(); });
        case MenuRow::Resume: return false; // (the command row's)
        default: return true;
        }
    };
    m_tabsShown.clear();
    for (size_t t = 0; t < m_config.tabs.size(); t++) {
        const MenuTab &tab = m_config.tabs[t];
        if (std::any_of(tab.pages.begin(), tab.pages.end(), [&](const std::string &page) { return TabPageShown(page); }) ||
            std::any_of(tab.rows.begin(), tab.rows.end(), rowShown)) {
            m_tabsShown.push_back(static_cast<int>(t));
        }
    }
    m_tabRow.resize(m_config.tabs.size(), 0);
    if (m_tabsShown.empty()) {
        m_page.clear();
        m_confirm = false;
        m_selected = 0;
        return;
    }
    m_tab = std::clamp(m_tab, 0, static_cast<int>(m_tabsShown.size()) - 1);
    const MenuTab &tab = m_config.tabs[static_cast<size_t>(Tab())];

    // (a page that is no longer there: the tab)
    if (!m_page.empty() && !Exists(m_page)) {
        m_page.clear();
        m_selected = m_left.empty() ? 0 : m_left.front();
        m_left.clear();
    }
    if (m_page.empty()) {
        // (each page under a heading of its name, where the tab has more than one)
        const bool headings = std::count_if(tab.pages.begin(), tab.pages.end(), [&](const std::string &page) { return TabPageShown(page); }) > 1;
        for (const std::string &page : tab.pages) {
            if (!TabPageShown(page)) {
                continue;
            }
            const size_t before = m_rows.size();
            PageRows(page, true);
            if (m_rows.size() > before && headings) {
                m_rows[before].heading = page;
            }
        }
        bool divider = !m_rows.empty();
        auto add = [&](Row row) {
            row.divider = divider;
            divider = false;
            m_rows.push_back(std::move(row));
        };
        for (const MenuRow &row : tab.rows) {
            const size_t index = m_first.size();
            m_first.push_back(row);
            if (!rowShown(row)) {
                continue;
            }
            switch (row.kind) {
            case MenuRow::Save: add({kRowSave, index, ""}); break;
            case MenuRow::Load: add({kRowLoad, index, ""}); break;
            case MenuRow::Slot: add({kRowSlot, index, ""}); break;
            case MenuRow::Speed: add({kRowSpeed, index, ""}); break;
            case MenuRow::Mute: add({kRowMute, index, ""}); break;
            case MenuRow::Action: add({kRowAction, index, ""}); break;
            case MenuRow::Quit: add({kRowQuit, index, ""}); break;
            case MenuRow::Reset: add({kRowReset, index, ""}); break;
            case MenuRow::ResetOptions: add({kRowResetOptions, index, ""}); break;
            case MenuRow::Page: add({kRowPage, index, row.title}); break;
            case MenuRow::Pages:
                for (size_t i = 0; i < m_config.commands.size(); i++) {
                    if (m_config.commands[i].category.empty()) {
                        add({kRowCommand, i, ""});
                    }
                }
                break;
            default: break;
            }
        }
    } else {
        PageRows(m_page, true);
    }
    // (Reset all options asked about: No and Yes alone)
    if (m_confirm) {
        if (m_confirmIndex < m_first.size() && m_first[m_confirmIndex].kind == MenuRow::ResetOptions) {
            m_rows.clear();
            m_rows.push_back({kRowNo, m_confirmIndex, ""});
            m_rows.push_back({kRowYes, m_confirmIndex, ""});
            m_selected = std::clamp(m_selected, 0, 1);
            return;
        }
        m_confirm = false;
        m_selected = m_confirmFrom;
    }
    m_selected = std::clamp(m_selected, 0, std::max(static_cast<int>(m_rows.size()) - 1, 0));
    if (m_rows.empty() && m_zone == kZoneRows) {
        m_zone = kZoneTabs;
    }
}

// To another tab (by its place among those shown), on the row it was left on
void Menu::ShowTab(int tab) {
    if (m_tabsShown.empty()) {
        return;
    }
    if (m_page.empty() && !m_confirm) {
        m_tabRow[static_cast<size_t>(Tab())] = m_selected;
    }
    const int count = static_cast<int>(m_tabsShown.size());
    m_tab = (tab % count + count) % count;
    m_page.clear();
    m_left.clear();
    m_confirm = false;
    m_selected = m_tabRow[static_cast<size_t>(Tab())];
    m_firstShown = 0;
    Fill();
}

Menu::Chosen Menu::ButtonsTabbed(unsigned pressed) {
    if (!m_open || !pressed) {
        return {};
    }
    m_note.clear();
    const int armed = m_armed;
    m_armed = -1;
    if (m_confirm && (pressed & (kMenuBack | kMenuLeft))) {
        EndConfirm();
        return {};
    }
    if (pressed & kMenuBack) {
        if (!m_page.empty()) {
            Leave();
            m_zone = kZoneRows;
        } else {
            m_open = false;
        }
        return {};
    }
    if (pressed & (kMenuTabPrev | kMenuTabNext)) {
        ShowTab(m_tab + ((pressed & kMenuTabNext) ? 1 : -1));
        m_zone = m_rows.empty() ? kZoneTabs : kZoneRows;
        return {};
    }
    switch (m_zone) {
    case kZoneBar: {
        const int count = static_cast<int>(m_bar.size());
        if (pressed & kMenuLeft) {
            m_barAt = std::max(m_barAt - 1, 0);
        } else if (pressed & kMenuRight) {
            m_barAt = std::min(m_barAt + 1, count - 1);
        } else if (pressed & kMenuDown) {
            m_zone = m_tabsShown.empty() ? kZoneBar : kZoneTabs;
        } else if (pressed & kMenuConfirm) {
            const BarItem item = m_bar[static_cast<size_t>(std::clamp(m_barAt, 0, count - 1))];
            switch (item.kind) {
            case kRowBack:
                if (!m_page.empty()) {
                    Leave();
                } else {
                    m_open = false;
                }
                break;
            case kRowResume: m_open = false; break;
            case kRowSave: return {Chosen::Save, 0};
            case kRowLoad: return {Chosen::Load, 0};
            case kRowSlot: SetSlot(m_slot + 1); break;
            case kRowMute: m_config.setMuted(!m_config.muted()); break;
            case kRowSpeed: m_speed = (m_speed + 1) % static_cast<int>(m_config.speeds.size()); break;
            case kRowAction: return {Chosen::Action, m_first[item.index].id};
            case kRowReset:
            case kRowQuit:
                // (asked a second time: they are a press away from losing the game as it is)
                if (armed == m_barAt) {
                    return {item.kind == kRowQuit ? Chosen::Quit : Chosen::Reset, 0};
                }
                m_armed = m_barAt;
                m_note = item.kind == kRowQuit ? i18n::TrFormat("{} again to quit the game.", {Tr(m_config.confirmName)})
                                               : i18n::TrFormat("{} again to reset the game.", {Tr(m_config.confirmName)});
                break;
            default: break;
            }
        }
        return {};
    }
    case kZoneTabs:
        if (pressed & (kMenuLeft | kMenuRight)) {
            ShowTab(m_tab + ((pressed & kMenuRight) ? 1 : -1));
        } else if (pressed & kMenuUp) {
            m_zone = kZoneBar;
        } else if ((pressed & (kMenuDown | kMenuConfirm)) && !m_rows.empty()) {
            m_zone = kZoneRows;
        }
        return {};
    default: break;
    }
    if (m_rows.empty()) {
        m_zone = kZoneTabs;
        return {};
    }
    const int count = static_cast<int>(m_rows.size());
    if (pressed & kMenuUp) {
        // (the top row: up to the tabs, then the command row)
        if (m_selected == 0) {
            m_zone = m_tabsShown.empty() ? kZoneBar : kZoneTabs;
        } else {
            m_selected--;
        }
        return {};
    }
    if (pressed & kMenuDown) {
        m_selected = (m_selected + 1) % count;
        return {};
    }
    if (pressed & (kMenuLeft | kMenuRight)) {
        // (a switch: Left off, Right on, as it is drawn)
        const Row &row = m_rows[static_cast<size_t>(m_selected)];
        const bool on = (pressed & kMenuRight) != 0;
        if (row.kind == kRowFeature && m_config.features[row.index].choices.empty() && m_config.setValue) {
            m_config.setValue(row.index, on ? 1 : 0);
            return {};
        }
        if (row.kind == kRowMute) {
            m_config.setMuted(on);
            return {};
        }
    }
    if (pressed & kMenuDefault) {
        const Row &row = m_rows[static_cast<size_t>(m_selected)];
        if (row.kind == kRowFeature && m_config.setValue) {
            m_config.setValue(row.index, FeatureDefault(row.index));
        } else if (row.kind == kRowSpeed) {
            SetSpeed(m_config.normalSpeed);
        } else if (row.kind == kRowMute && m_config.muted()) {
            m_config.setMuted(false);
        }
        return {};
    }
    return RowButtons(pressed);
}

namespace {
    enum { kHitBar, kHitTab, kHitTabPrev, kHitTabNext, kHitRow, kHitPrev, kHitNext, kHitChoice, kHitList };
} // namespace

Menu::Chosen Menu::MouseTabbed(MenuMouse what, float x, float y) {
    if (!m_open) {
        return {};
    }
    if (what == MenuMouse::Back) {
        return ButtonsTabbed(kMenuBack);
    }
    // (what is under the mouse, as last drawn: the last of those that hold it, the smallest)
    const Hit *over = nullptr;
    for (const Hit &hit : m_hits) {
        if (x >= hit.x && x < hit.x + hit.w && y >= hit.y && y < hit.y + hit.h) {
            over = &hit;
        }
    }
    if (what == MenuMouse::WheelUp || what == MenuMouse::WheelDown) {
        // (the wheel goes through the rows wherever it is, but over the tabs, which it goes through)
        if (over && (over->what == kHitTab || over->what == kHitTabPrev || over->what == kHitTabNext)) {
            ShowTab(m_tab + (what == MenuMouse::WheelDown ? 1 : -1));
            return {};
        }
        if (!m_rows.empty()) {
            m_zone = kZoneRows;
            m_selected = std::clamp(m_selected + (what == MenuMouse::WheelDown ? 1 : -1), 0, static_cast<int>(m_rows.size()) - 1);
        }
        return {};
    }
    if (!over) {
        return {};
    }
    const bool click = what == MenuMouse::Click;
    switch (over->what) {
    case kHitBar:
        if (m_barAt != over->index) {
            m_armed = -1;
        }
        m_zone = kZoneBar;
        m_barAt = over->index;
        return click ? ButtonsTabbed(kMenuConfirm) : Chosen{};
    case kHitTab:
        if (click) {
            ShowTab(over->index);
            m_zone = kZoneTabs;
        }
        return {};
    case kHitTabPrev:
    case kHitTabNext:
        if (click) {
            ShowTab(m_tab + (over->what == kHitTabNext ? 1 : -1));
            m_zone = kZoneTabs;
        }
        return {};
    case kHitRow:
    case kHitPrev:
    case kHitNext:
        if (over->index >= static_cast<int>(m_rows.size())) {
            return {};
        }
        m_zone = kZoneRows;
        m_selected = over->index;
        if (!click) {
            return {};
        }
        m_note.clear();
        return over->what == kHitRow ? RowButtons(kMenuConfirm) : RowButtons(over->what == kHitPrev ? kMenuLeft : kMenuRight);
    case kHitChoice: {
        if (!click || m_rows.empty()) {
            return {};
        }
        const Row &row = m_rows[static_cast<size_t>(m_selected)];
        if (row.kind == kRowFeature && m_config.setValue) {
            m_config.setValue(row.index, over->index);
        } else if (row.kind == kRowSpeed) {
            SetSpeed(over->index);
        }
        return {};
    }
    default: return {};
    }
}

namespace {
    using text::Canvas;
    using text::Color;

    uint32_t Packed(Color c) {
        return static_cast<uint32_t>(c.a) << 24 | static_cast<uint32_t>(c.b) << 16 | static_cast<uint32_t>(c.g) << 8 | c.r;
    }

    // A rectangle with its corners rounded by r
    void FillRound(const Canvas &canvas, float x, float y, float w, float h, float r, Color c) {
        r = std::min({r, w / 2, h / 2});
        if (r < 1) {
            canvas.Fill(x, y, w, h, c);
            return;
        }
        const int rows = static_cast<int>(std::ceil(r));
        for (int i = 0; i < rows; i++) {
            const float dy = r - (i + 0.5f);
            const float inset = r - std::sqrt(std::max(r * r - dy * dy, 0.0f));
            canvas.Fill(std::round(x + inset), y + i, std::round(w - 2 * inset), 1, c);
            canvas.Fill(std::round(x + inset), y + h - i - 1, std::round(w - 2 * inset), 1, c);
        }
        canvas.Fill(x, y + rows, w, h - 2 * rows, c);
    }

    // A frame of thickness t inside the rectangle
    void Ring(const Canvas &canvas, float x, float y, float w, float h, float t, Color c) {
        canvas.Fill(x, y, w, t, c);
        canvas.Fill(x, y + h - t, w, t, c);
        canvas.Fill(x, y + t, t, h - 2 * t, c);
        canvas.Fill(x + w - t, y + t, t, h - 2 * t, c);
    }

    // Text in the host's font or the dot font: sizes are the dot font's dots (a capital is 7 of them)
    struct Type {
        const Canvas &canvas;
        const recomp::MenuFont *font;
        float Width(const std::string &s, float dot) const {
            return font ? font->Width(s.c_str(), 7 * dot) : text::TextWidth(s.c_str(), dot);
        }
        void Draw(float x, float y, float dot, Color c, const std::string &s) const {
            if (font) {
                font->Draw(canvas.pixels, canvas.width, canvas.height, x, y, 7 * dot, Packed(c), s.c_str());
            } else {
                canvas.Chars(std::round(x), std::round(y), dot, s.c_str(), s.size(), c);
            }
        }
        // Lines of at most `width`, broken at spaces
        std::vector<std::string> Wrap(const std::string &s, float dot, float width) const {
            std::vector<std::string> lines;
            std::string line;
            size_t at = 0;
            while (at < s.size()) {
                size_t end = s.find(' ', at);
                if (end == std::string::npos) {
                    end = s.size();
                }
                const std::string word = s.substr(at, end - at);
                const std::string longer = line.empty() ? word : line + " " + word;
                if (!line.empty() && Width(longer, dot) > width) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = longer;
                }
                at = end + 1;
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
            return lines;
        }
        // Cut short with ".." to fit `width`
        std::string Fit(std::string s, float dot, float width) const {
            if (Width(s, dot) <= width) {
                return s;
            }
            while (s.size() > 1 && Width(s + "..", dot) > width) {
                s.pop_back();
                while (s.size() > 1 && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
                    s.pop_back();
                }
            }
            return s + "..";
        }
    };

    // In capitals (the CRT look's): ASCII's letters and Latin-1's accented ones (UTF-8 C3 A0-BE, but for the sign of
    // division, F7)
    std::string Capitals(std::string s) {
        for (size_t i = 0; i < s.size(); i++) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 'a' && c <= 'z') {
                s[i] = static_cast<char>(c - 'a' + 'A');
            } else if (c == 0xC3 && i + 1 < s.size()) {
                const unsigned char next = static_cast<unsigned char>(s[i + 1]);
                if (next >= 0xA0 && next <= 0xBE && next != 0xB7) {
                    s[i + 1] = static_cast<char>(next - 0x20);
                }
                i++;
            }
        }
        return s;
    }

    // A look: its sizes and colours. Everything else is the same in both.
    struct Look {
        bool crt;
        float u;          // a unit: a pixel of the window look at 720 lines, a board pixel of the CRT look
        float dot, small; // the dot font's dot for the rows and for the small text
        float rowHeight, headingHeight, barHeight, tabsHeight, footHeight;
        Color shade, panel, strip, line, textColor, dim, accent, onAccent, value, changed, danger, dangerFill, focusFill, pane;
    };

    Look WindowLook(int height) {
        const float u = std::max(height / 720.0f, 0.5f);
        Look look{};
        look.crt = false;
        look.u = u;
        look.dot = std::max(1.0f, std::round(2 * u));
        look.small = look.dot;
        look.rowHeight = 38 * u;
        look.headingHeight = 30 * u;
        look.barHeight = 60 * u;
        look.tabsHeight = 48 * u;
        look.footHeight = 40 * u;
        look.shade = {7, 9, 12, 170};
        look.panel = {13, 16, 22, 246};
        look.strip = {15, 19, 26, 255};
        look.line = {37, 44, 56, 255};
        look.textColor = {232, 236, 242, 255};
        look.dim = {141, 151, 168, 255};
        look.accent = {124, 196, 255, 255};
        look.onAccent = {8, 17, 28, 255};
        look.value = {159, 211, 255, 255};
        look.changed = {124, 196, 255, 255};
        look.danger = {240, 176, 168, 255};
        look.dangerFill = {92, 38, 36, 255};
        look.focusFill = {26, 36, 49, 255};
        look.pane = {15, 19, 26, 255};
        return look;
    }

    Look CrtLook(int height) {
        // (whole board pixels: a 224 or 240 line screen has 1, a picture made twice the board's 2)
        const float u = std::max(1.0f, std::floor(height / 224.0f));
        Look look{};
        look.crt = true;
        look.u = u;
        look.dot = u;
        look.small = u;
        look.rowHeight = 11 * u;
        look.headingHeight = 9 * u;
        look.barHeight = 14 * u;
        look.tabsHeight = 16 * u;
        look.footHeight = 13 * u;
        look.shade = {0, 0, 0, 110};
        look.panel = {16, 24, 74, 255}; // (navy: a white field blooms on a CRT)
        look.strip = {27, 38, 104, 255};
        look.line = {44, 58, 150, 255};
        look.textColor = {244, 244, 244, 255};
        look.dim = {143, 155, 214, 255};
        look.accent = {255, 210, 63, 255};
        look.onAccent = {16, 24, 74, 255};
        look.value = {127, 227, 255, 255};
        look.changed = {255, 210, 63, 255};
        look.danger = {255, 154, 138, 255};
        look.dangerFill = {150, 40, 32, 255};
        look.focusFill = {255, 210, 63, 255};
        look.pane = {16, 24, 74, 255};
        return look;
    }
} // namespace

void Menu::DrawTabbed(uint32_t *pixels, int width, int height) const {
    m_hits.clear();
    const Canvas canvas{pixels, width, height};
    const Look look = m_compact ? CrtLook(height) : WindowLook(height);
    const Type type{canvas, m_compact ? nullptr : m_font};
    const float u = look.u;
    const bool crt = look.crt;
    const auto words = [crt](std::string s) { return crt ? Capitals(std::move(s)) : s; };
    // (the dot font's letters: 7 dots high, set in the middle of a box of height h at y)
    const auto middle = [](float y, float h, float dot) { return std::round(y + (h - 7 * dot) / 2); };

    // The panel: the window look's a card over the darkened game, the CRT look's the whole screen but its overscan
    float px, py, pw, ph;
    if (crt) {
        px = std::round(width * 0.03f);
        py = std::round(height * 0.035f);
        pw = width - 2 * px;
        ph = height - 2 * py;
    } else {
        pw = std::min(width - 32 * u, 1200 * u);
        ph = height - 48 * u;
        px = std::round((width - pw) / 2);
        py = std::round((height - ph) / 2);
    }
    canvas.Fill(0, 0, static_cast<float>(width), static_cast<float>(height), look.shade);
    if (crt) {
        canvas.Fill(px, py, pw, ph, look.panel);
    } else {
        FillRound(canvas, px, py, pw, ph, 14 * u, look.panel);
    }

    // The command row: Back, the host's, Resume
    {
        const float y = py, h = look.barHeight;
        if (crt) {
            canvas.Fill(px, y, pw, h, look.strip);
        }
        // (each item's words: short ones where the long do not fit)
        const auto label = [&](const BarItem &item, bool shortly) -> std::string {
            switch (item.kind) {
            case kRowBack: return words("< " + std::string(Tr("Back")));
            case kRowResume: return words(Tr("Resume"));
            case kRowSave: return words(shortly ? Tr("Save") : Tr("Save state"));
            case kRowLoad: return words(shortly ? Tr("Load") : Tr("Load state"));
            case kRowSlot: return crt || shortly ? "<" + std::to_string(m_slot) + ">" : std::string(Tr("Slot")) + "  < " + std::to_string(m_slot) + " >";
            case kRowReset: return words(shortly ? Tr("Reset") : Tr("Reset game"));
            case kRowQuit: return words(shortly ? Tr("Quit") : Tr("Quit game"));
            case kRowMute: return words(std::string(Tr("Mute")) + " " + Tr(m_config.muted() ? "On" : "Off"));
            case kRowSpeed: return words(std::string(Tr("Speed")) + " " + Tr(m_config.speeds[static_cast<size_t>(m_speed)]));
            case kRowAction: return words(Tr(m_first[item.index].title));
            default: return "";
            }
        };
        const float itemPad = crt ? 3 * u : 14 * u, gap = crt ? 4 * u : 4 * u, edge = crt ? 2 * u : 10 * u;
        const float itemHeight = crt ? h - 2 * u : 42 * u;
        const auto widths = [&](bool shortly) {
            std::vector<float> w;
            for (const BarItem &item : m_bar) {
                w.push_back(type.Width(label(item, shortly), look.dot) + 2 * itemPad);
            }
            return w;
        };
        bool shortly = false;
        std::vector<float> w = widths(false);
        float total = 0;
        for (float one : w) {
            total += one + gap;
        }
        if (total + 2 * edge + (crt ? 0 : 40 * u) > pw) {
            shortly = true;
            w = widths(true);
        }
        float middleWidth = 0;
        for (size_t i = 1; i + 1 < m_bar.size(); i++) {
            middleWidth += w[i] + (i + 2 < m_bar.size() ? gap : 0);
        }
        const float left = px + edge + w.front(), right = px + pw - edge - w.back();
        float mx = std::round(left + std::max((right - left - middleWidth) / 2, gap));
        for (size_t i = 0; i < m_bar.size(); i++) {
            const BarItem &item = m_bar[i];
            float ix;
            if (i == 0) {
                ix = px + edge;
            } else if (i + 1 == m_bar.size()) {
                ix = px + pw - edge - w[i];
            } else {
                ix = mx;
                mx += w[i] + gap;
            }
            const float iy = std::round(y + (h - itemHeight) / 2);
            const bool focused = m_zone == kZoneBar && m_barAt == static_cast<int>(i);
            const bool armed = m_armed == static_cast<int>(i);
            const bool resume = item.kind == kRowResume;
            const bool back = item.kind == kRowBack;
            Color fill{0, 0, 0, 0}, ink = item.kind == kRowQuit ? look.danger : look.textColor;
            if (back && m_page.empty()) {
                ink = look.dim; // (nothing to go back to: it closes)
            }
            if (resume && !crt) {
                fill = look.accent;
                ink = look.onAccent;
            }
            if (armed) {
                fill = look.dangerFill;
                ink = {255, 255, 255, 255};
            } else if (focused && crt) {
                fill = look.accent;
                ink = look.onAccent;
            } else if (focused) {
                fill = resume ? look.accent : Color{34, 42, 55, 255};
            }
            if (fill.a) {
                if (crt) {
                    canvas.Fill(ix, iy, w[i], itemHeight, fill);
                } else {
                    FillRound(canvas, ix, iy, w[i], itemHeight, 10 * u, fill);
                }
            }
            if (focused && !crt) {
                Ring(canvas, ix, iy, w[i], itemHeight, std::max(1.0f, std::round(2 * u)), resume ? Color{255, 255, 255, 255} : look.accent);
            }
            if (resume && crt && !focused && !armed) {
                ink = look.accent;
            }
            type.Draw(ix + itemPad, middle(iy, itemHeight, look.dot), look.dot, ink, label(item, shortly));
            m_hits.push_back({kHitBar, static_cast<int>(i), ix, iy, w[i], itemHeight});
        }
        if (!crt) {
            canvas.Fill(px, y + h - 1, pw, std::max(1.0f, std::round(u)), look.line);
        }
    }

    // The tabs: a strip of them, or in the CRT look the one shown between arrows
    const float tabsY = py + look.barHeight;
    const auto changesOf = [&](int tab) {
        // (the features of its pages and of the pages under them)
        const MenuTab &of = m_config.tabs[static_cast<size_t>(tab)];
        int count = 0;
        for (size_t f = 0; f < m_config.features.size(); f++) {
            const std::string &category = m_config.features[f].category;
            bool in = false;
            for (const std::string &page : of.pages) {
                in = in || category == page || Parent(category) == page || Parent(Parent(category)) == page;
            }
            count += in && Changed(f);
        }
        return count;
    };
    if (!m_tabsShown.empty()) {
        const float h = look.tabsHeight;
        if (crt) {
            const std::string name = words(Tr(m_config.tabs[static_cast<size_t>(Tab())].name));
            const std::string place = std::to_string(m_tab + 1) + "/" + std::to_string(m_tabsShown.size());
            const float nameWidth = type.Width(name, look.dot);
            const float cx = std::round(px + (pw - nameWidth) / 2);
            const float ty = middle(tabsY, h, look.dot);
            const bool focused = m_zone == kZoneTabs;
            if (focused) {
                canvas.Fill(cx - 3 * u, tabsY + 2 * u, nameWidth + 6 * u, h - 4 * u, look.accent);
            }
            type.Draw(cx, ty, look.dot, focused ? look.onAccent : look.textColor, name);
            const float arrow = type.Width("<", look.dot);
            type.Draw(cx - 12 * u - arrow, ty, look.dot, look.accent, "<");
            type.Draw(cx + nameWidth + 12 * u, ty, look.dot, look.accent, ">");
            m_hits.push_back({kHitTabPrev, 0, cx - 16 * u - arrow, tabsY, arrow + 12 * u, h});
            m_hits.push_back({kHitTab, m_tab, cx - 3 * u, tabsY, nameWidth + 6 * u, h});
            m_hits.push_back({kHitTabNext, 0, cx + nameWidth + 4 * u, tabsY, arrow + 12 * u, h});
            type.Draw(px + pw - 4 * u - type.Width(place, look.dot), ty, look.dot, look.dim, place);
            canvas.Fill(px + 4 * u, tabsY + h - 2 * u, pw - 8 * u, 2 * u, look.line);
        } else {
            canvas.Fill(px, tabsY, pw, h, look.strip);
            const float tabPad = 14 * u;
            std::vector<float> tabWidth;
            std::vector<std::string> badge;
            float all = 0;
            for (size_t i = 0; i < m_tabsShown.size(); i++) {
                const int changes = changesOf(m_tabsShown[i]);
                badge.push_back(changes ? std::to_string(changes) : "");
                tabWidth.push_back(type.Width(Tr(m_config.tabs[static_cast<size_t>(m_tabsShown[i])].name), look.dot) +
                                   (changes ? type.Width(badge.back(), look.small) + 16 * u : 0) + 2 * tabPad);
                all += tabWidth.back();
            }
            // (the strip slid along, when the tabs are wider than it, so that the one shown is in it)
            const float stripLeft = px + 12 * u, stripWidth = pw - 24 * u;
            float shift = 0;
            if (all > stripWidth) {
                float before = 0;
                for (int i = 0; i < m_tab; i++) {
                    before += tabWidth[static_cast<size_t>(i)];
                }
                shift = std::clamp(before + tabWidth[static_cast<size_t>(m_tab)] / 2 - stripWidth / 2, 0.0f, all - stripWidth);
            }
            float tx = stripLeft - shift;
            for (size_t i = 0; i < m_tabsShown.size(); i++) {
                const bool shown = static_cast<int>(i) == m_tab;
                const bool focused = shown && m_zone == kZoneTabs;
                if (tx + tabWidth[i] > stripLeft && tx < stripLeft + stripWidth) {
                    if (focused) {
                        FillRound(canvas, tx + 2 * u, tabsY + 6 * u, tabWidth[i] - 4 * u, h - 12 * u, 8 * u, look.focusFill);
                        Ring(canvas, tx + 2 * u, tabsY + 6 * u, tabWidth[i] - 4 * u, h - 12 * u, std::max(1.0f, std::round(2 * u)), look.accent);
                    }
                    const std::string name = Tr(m_config.tabs[static_cast<size_t>(m_tabsShown[i])].name);
                    type.Draw(tx + tabPad, middle(tabsY, h, look.dot), look.dot, shown ? Color{255, 255, 255, 255} : look.dim, name);
                    if (!badge[i].empty()) {
                        const float bx = tx + tabPad + type.Width(name, look.dot) + 8 * u;
                        const float bw = type.Width(badge[i], look.small) + 10 * u, bh = 9 * look.small;
                        FillRound(canvas, bx, std::round(tabsY + (h - bh) / 2), bw, bh, bh / 2, {29, 51, 71, 255});
                        type.Draw(bx + 5 * u, middle(tabsY, h, look.small), look.small, look.value, badge[i]);
                    }
                    if (shown) {
                        canvas.Fill(tx + tabPad / 2, tabsY + h - 3 * u, tabWidth[i] - tabPad, std::round(3 * u), look.accent);
                    }
                    m_hits.push_back({kHitTab, static_cast<int>(i), tx, tabsY, tabWidth[i], h});
                }
                tx += tabWidth[i];
            }
            canvas.Fill(px, tabsY + h - 1, pw, std::max(1.0f, std::round(u)), look.line);
        }
    }

    // Under the tabs: the rows, and what the row chosen is (beside them in a wide window, under them otherwise)
    const float top = tabsY + (m_tabsShown.empty() ? 0 : look.tabsHeight);
    const float bottom = py + ph - look.footHeight;
    const bool side = !crt && pw >= 820 * u;
    const float paneWidth = side ? std::round(pw * 0.32f) : 0;
    const float listX = px + (crt ? 4 * u : 14 * u), listWidth = pw - paneWidth - (crt ? 8 * u : 28 * u);
    std::string title, value, description;
    int valueKind = kValuePlain; // (the pane shows the choices themselves)
    const Row *chosen = m_rows.empty() ? nullptr : &m_rows[static_cast<size_t>(std::clamp(m_selected, 0, static_cast<int>(m_rows.size()) - 1))];
    if (m_zone == kZoneBar && !m_bar.empty()) {
        // (the command row's item: what it does)
        const BarItem &item = m_bar[static_cast<size_t>(std::clamp(m_barAt, 0, static_cast<int>(m_bar.size()) - 1))];
        Describe({item.kind, item.index, ""}, title, value, valueKind, description);
        if (item.kind == kRowBack) {
            description = m_page.empty() ? Tr("Back to the game.")
                                         : i18n::TrFormat("To the {} page (Left does the same).",
                                                          {Tr(InTab(Parent(m_page)) || Parent(m_page).empty()
                                                                  ? m_config.tabs[static_cast<size_t>(Tab())].name
                                                                  : Parent(m_page))});
        }
        chosen = nullptr; // (no choices shown)
    } else if (m_zone == kZoneTabs) {
        chosen = nullptr;
    } else if (chosen) {
        Describe(*chosen, title, value, valueKind, description);
    }
    const std::string note = Tr(m_note);
    // (under the rows, the description's lines: three in the window look, two or three in the CRT look)
    const int belowLines = side ? 0 : crt ? (ph / u > 200 ? 3 : 2) : 3;
    const float belowHeight = belowLines ? belowLines * text::kLineHeight * look.small + (crt ? 6 * u : 16 * u) : 0;
    float y = top + (crt ? 3 * u : 8 * u);
    if (!m_page.empty() && !m_confirm) {
        // (where in the tab: Training > Dummy)
        std::string path = Tr(m_page);
        for (std::string at = Parent(m_page); !at.empty() && !InTab(at); at = Parent(at)) {
            path = std::string(Tr(at)) + " > " + path;
        }
        path = words(std::string(m_tabsShown.empty() ? "" : Tr(m_config.tabs[static_cast<size_t>(Tab())].name)) + " > " + path);
        type.Draw(listX + (crt ? 4 * u : 10 * u), y + (crt ? 0 : 4 * u), look.small, look.dim, type.Fit(path, look.small, listWidth));
        y += crt ? 10 * u : 28 * u;
    } else if (m_confirm) {
        type.Draw(listX + (crt ? 4 * u : 10 * u), y + (crt ? 0 : 4 * u), look.small, look.danger, words(Tr("Reset all options: are you sure?")));
        y += crt ? 10 * u : 28 * u;
    }
    const float listTop = y, listBottom = bottom - belowHeight - (crt ? 2 * u : 6 * u);
    // (each row's height, with its heading and its line; those about the one chosen shown when they do not all fit)
    const auto heightOf = [&](size_t i) {
        const Row &row = m_rows[i];
        return look.rowHeight + (row.heading.empty() ? 0 : look.headingHeight) + (row.divider && row.heading.empty() ? (crt ? 5 : 13) * u : 0);
    };
    const int count = static_cast<int>(m_rows.size());
    m_firstShown = std::clamp(m_firstShown, 0, std::max(count - 1, 0));
    if (m_selected < m_firstShown) {
        m_firstShown = m_selected;
    }
    while (m_firstShown < m_selected) {
        float need = 0;
        for (int i = m_firstShown; i <= m_selected; i++) {
            need += heightOf(static_cast<size_t>(i));
        }
        if (need <= listBottom - listTop) {
            break;
        }
        m_firstShown++;
    }
    m_hits.push_back({kHitList, 0, listX, listTop, listWidth, listBottom - listTop});
    int lastShown = m_firstShown - 1;
    for (int i = m_firstShown; i < count; i++) {
        const Row &row = m_rows[static_cast<size_t>(i)];
        if (y + heightOf(static_cast<size_t>(i)) > listBottom + 0.5f) {
            break;
        }
        lastShown = i;
        if (!row.heading.empty()) {
            const std::string heading = Capitals(Tr(row.heading));
            if (crt) {
                canvas.SmallText(listX + 4 * u, y + 2 * u, u, look.dim, heading.c_str());
            } else {
                canvas.SmallText(listX + 10 * u, std::round(y + look.headingHeight - 13 * u), look.small, look.dim, heading.c_str());
            }
            y += look.headingHeight;
        } else if (row.divider) {
            const float lh = (crt ? 5 : 13) * u;
            canvas.Fill(listX + 4 * u, std::round(y + lh / 2), listWidth - 8 * u, crt ? 2 * u : std::max(1.0f, std::round(u)), look.line);
            y += lh;
        }
        const float h = look.rowHeight;
        const bool selected = i == m_selected;
        const bool focused = selected && m_zone == kZoneRows;
        std::string rowTitle, rowValue, rowDescription;
        int kind = kValuePlain;
        Describe(row, rowTitle, rowValue, kind, rowDescription);
        rowTitle = words(rowTitle);
        const bool isFeature = row.kind == kRowFeature;
        const bool changed = isFeature && Changed(row.index);
        const bool toggle = (isFeature && m_config.features[row.index].choices.empty()) || row.kind == kRowMute;
        const bool choice = (isFeature && !toggle) || row.kind == kRowSpeed || row.kind == kRowSlot || row.kind == kRowFavourites;
        const bool danger = row.kind == kRowResetOptions || row.kind == kRowQuit || row.kind == kRowYes;
        Color ink = danger ? look.danger : look.textColor;
        if (focused && crt) {
            canvas.Fill(listX, y, listWidth, h, look.focusFill);
            ink = look.onAccent;
        } else if (focused) {
            FillRound(canvas, listX, y + u, listWidth, h - 2 * u, 8 * u, look.focusFill);
            Ring(canvas, listX, y + u, listWidth, h - 2 * u, std::max(1.0f, std::round(2 * u)), look.accent);
        } else if (selected && !crt) {
            FillRound(canvas, listX, y + u, listWidth, h - 2 * u, 8 * u, {20, 26, 35, 255});
        }
        const float ty = middle(y, h, look.dot);
        float tx = listX + (crt ? 4 * u : 22 * u);
        if (changed) {
            if (crt) {
                type.Draw(listX + u, ty, look.dot, focused ? look.onAccent : look.changed, "*");
                tx = listX + 4 * u + type.Width("*", look.dot);
            } else {
                FillRound(canvas, listX + 9 * u, std::round(y + h / 2 - 3.5f * u), 7 * u, 7 * u, 3.5f * u, look.changed);
            }
        }
        // (the row's own place first, so that its arrows and its switch, after it, win)
        m_hits.push_back({kHitRow, i, listX, y, listWidth, h});
        // The value: < choice > (its arrows the mouse's), a switch, a page's count and arrow
        float valueLeft = listX + listWidth;
        const float right = listX + listWidth - (crt ? 4 * u : 8 * u);
        if (toggle) {
            const bool on = isFeature ? (m_config.value && m_config.value(row.index) != 0) : m_config.muted();
            if (crt) {
                const std::string said = words(Tr(on ? "On" : "Off"));
                const float vw = type.Width(said, look.dot);
                type.Draw(right - vw, ty, look.dot, focused ? look.onAccent : on ? look.value : look.dim, said);
                valueLeft = right - vw;
            } else {
                const float tw = 44 * u, th = 24 * u, tX = right - tw - 4 * u, tY = std::round(y + (h - th) / 2);
                FillRound(canvas, tX, tY, tw, th, th / 2, on ? look.accent : Color{42, 49, 64, 255});
                const float knob = 18 * u;
                FillRound(canvas, on ? tX + tw - knob - 3 * u : tX + 3 * u, tY + 3 * u, knob, knob, knob / 2, on ? look.onAccent : look.dim);
                valueLeft = tX;
            }
            m_hits.push_back({kHitNext, i, valueLeft - 6 * u, y, right - valueLeft + 12 * u, h});
        } else if (choice || row.kind == kRowPage) {
            std::string said = rowValue;
            if (row.kind == kRowPage) {
                // (a page's count: its features changed from their defaults)
                int changes = 0;
                for (size_t f = 0; f < m_config.features.size(); f++) {
                    const std::string &category = m_config.features[f].category;
                    changes += (category == row.page || Parent(category) == row.page) && Changed(f);
                }
                said = changes ? i18n::TrFormat("{} changed", {std::to_string(changes)}) : "";
            }
            said = words(said);
            const float arrowWidth = type.Width(">", look.dot);
            const float boxWidth = crt ? 0 : 150 * u;
            const float room = std::max(listWidth * 0.55f, 40 * u);
            said = type.Fit(said, look.dot, room - 2 * arrowWidth - (crt ? 8 * u : 24 * u));
            const float vw = type.Width(said, look.dot);
            Color valueInk = focused && crt ? look.onAccent : (changed || kind == kValueOn) ? look.value : look.textColor;
            if (row.kind == kRowPage) {
                type.Draw(right - arrowWidth, ty, look.dot, focused && crt ? look.onAccent : look.dim, ">");
                type.Draw(right - arrowWidth - (crt ? 6 : 12) * u - vw, ty, look.small, focused && crt ? look.onAccent : look.dim, said);
                valueLeft = right - arrowWidth - 12 * u - vw;
            } else {
                const bool arrows = !crt || focused;
                const float inner = std::max(vw, boxWidth);
                // (the CRT look's arrows only on the row chosen: the others' values at the edge, as a switch's are)
                const float nextX = right - arrowWidth, valueX = arrows ? nextX - (crt ? 4 * u : 10 * u) - inner : right - inner;
                const float prevX = valueX - (crt ? 4 * u : 10 * u) - arrowWidth;
                type.Draw(valueX + (inner - vw) / 2, ty, look.dot, valueInk, said);
                if (arrows) {
                    const Color arrowInk = focused ? (crt ? look.onAccent : look.accent) : look.dim;
                    type.Draw(prevX, ty, look.dot, arrowInk, "<");
                    type.Draw(nextX, ty, look.dot, arrowInk, ">");
                }
                valueLeft = prevX;
                m_hits.push_back({kHitPrev, i, prevX - 6 * u, y, arrowWidth + 10 * u, h});
                m_hits.push_back({kHitNext, i, nextX - 4 * u, y, arrowWidth + 10 * u, h});
            }
        }
        type.Draw(tx, ty, look.dot, ink, type.Fit(rowTitle, look.dot, valueLeft - tx - 8 * u));
        y += h;
    }
    // (rows above or below those shown: a bar at the side, as long as the part shown)
    if (count > 0 && (m_firstShown > 0 || lastShown < count - 1)) {
        const float trackTop = listTop, trackHeight = listBottom - listTop, barWidth = crt ? 2 * u : 4 * u;
        const float barX = listX + listWidth + (crt ? u : 4 * u);
        canvas.Fill(barX, trackTop, barWidth, trackHeight, {255, 255, 255, 24});
        const float from = static_cast<float>(m_firstShown) / count, to = static_cast<float>(lastShown + 1) / count;
        canvas.Fill(barX, trackTop + from * trackHeight, barWidth, std::max((to - from) * trackHeight, 4 * u), look.dim);
    }

    // What the row chosen is: its name, what it does, and its choices where they fit
    const std::string said = note.empty() ? description : note;
    const Color saidInk = note.empty() ? (crt ? Color{215, 220, 245, 255} : Color{184, 194, 210, 255}) : look.accent;
    if (side) {
        const float sx = px + pw - paneWidth, sy = top;
        canvas.Fill(sx, sy, paneWidth, bottom - sy, look.pane);
        canvas.Fill(sx, sy, std::max(1.0f, std::round(u)), bottom - sy, look.line);
        const float inner = paneWidth - 48 * u, ix = sx + 24 * u;
        float yy = sy + 22 * u;
        if (!title.empty()) {
            for (const std::string &line : type.Wrap(title, look.dot + (look.dot >= 2 ? 1 : 0), inner)) {
                type.Draw(ix, yy, look.dot + (look.dot >= 2 ? 1 : 0), {255, 255, 255, 255}, line);
                yy += text::kLineHeight * (look.dot + (look.dot >= 2 ? 1 : 0));
            }
            yy += 8 * u;
        }
        for (const std::string &line : type.Wrap(said, look.small, inner)) {
            if (yy + text::kLineHeight * look.small > bottom - 12 * u) {
                break;
            }
            type.Draw(ix, yy, look.small, saidInk, line);
            yy += text::kLineHeight * look.small + 2 * u;
        }
        // (a choice's names, the one chosen marked, the default said; those about the chosen one where they are many)
        std::vector<std::string> names;
        int now = 0, byDefault = -1;
        if (chosen && chosen->kind == kRowFeature && !m_config.features[chosen->index].choices.empty()) {
            const MenuFeature &feature = m_config.features[chosen->index];
            names = feature.choices;
            now = m_config.value ? m_config.value(chosen->index) : 0;
            byDefault = FeatureDefault(chosen->index);
        } else if (chosen && chosen->kind == kRowSpeed) {
            names = m_config.speeds;
            now = m_speed;
            byDefault = m_config.normalSpeed;
        }
        if (!names.empty()) {
            yy += 14 * u;
            const float lineHeight = 32 * u;
            const int fit = std::max(1, static_cast<int>((bottom - 16 * u - yy) / lineHeight));
            std::vector<int> offered;
            for (int c = 0; c < static_cast<int>(names.size()); c++) {
                if (c == 0 || c == now || chosen->kind != kRowFeature || !m_config.offered || m_config.offered(chosen->index, c)) {
                    offered.push_back(c);
                }
            }
            const int at = static_cast<int>(std::find(offered.begin(), offered.end(), now) - offered.begin());
            const int count2 = static_cast<int>(offered.size());
            const int first = std::clamp(at - fit / 2, 0, std::max(count2 - fit, 0));
            for (int k = first; k < std::min(count2, first + fit); k++, yy += lineHeight) {
                const int c = offered[static_cast<size_t>(k)];
                const bool is = c == now;
                if (is) {
                    FillRound(canvas, ix - 10 * u, yy, inner + 20 * u, lineHeight - 4 * u, 7 * u, {29, 51, 71, 255});
                }
                const std::string name = Tr(names[static_cast<size_t>(c)]);
                const float my = middle(yy, lineHeight - 4 * u, look.small);
                const std::string tag = c == byDefault ? std::string(Tr("default")) : "";
                const float tagWidth = tag.empty() ? 0 : type.Width(tag, look.small) + 8 * u;
                type.Draw(ix, my, look.small, is ? Color{255, 255, 255, 255} : Color{184, 194, 210, 255}, type.Fit(name, look.small, inner - tagWidth));
                if (!tag.empty()) {
                    type.Draw(ix + inner - tagWidth + 8 * u, my, look.small, look.dim, tag);
                }
                m_hits.push_back({kHitChoice, c, ix - 10 * u, yy, inner + 20 * u, lineHeight - 4 * u});
            }
        }
    } else if (belowLines) {
        const float by = bottom - belowHeight;
        canvas.Fill(px + 4 * u, by, pw - 8 * u, crt ? 2 * u : std::max(1.0f, std::round(u)), look.line);
        const std::vector<std::string> lines = type.Wrap(words(said), look.small, pw - (crt ? 16 : 40) * u);
        for (size_t l = 0; l < lines.size() && l < static_cast<size_t>(belowLines); l++) {
            const bool last = l + 1 == static_cast<size_t>(belowLines) && lines.size() > l + 1;
            type.Draw(px + (crt ? 8 : 20) * u, by + (crt ? 4 : 10) * u + l * text::kLineHeight * look.small, look.small, saidInk,
                      last ? type.Fit(lines[l] + " " + lines[l + 1], look.small, pw - (crt ? 16 : 40) * u) : lines[l]);
        }
    }

    // The hint line: what the buttons do
    {
        const float fy = bottom, h = look.footHeight;
        if (crt) {
            canvas.Fill(px, fy, pw, h, look.strip);
        } else {
            canvas.Fill(px, fy, pw, std::max(1.0f, std::round(u)), look.line);
        }
        std::string hint = crt ? Capitals(i18n::TrFormat("{}: back   {}: tab", {Tr(m_config.backName), Tr(m_config.tabsName)}))
                               : i18n::TrFormat("Up/Down: choose   Left/Right: change   {}: tab   {}: back   {}: close",
                                                {Tr(m_config.tabsName), Tr(m_config.backName), Tr(m_config.closeName)});
        if (!crt && type.Width(hint, look.small) + 44 * u > pw) {
            hint = i18n::TrFormat("{}: tab   {}: back   {}: close", {Tr(m_config.tabsName), Tr(m_config.backName), Tr(m_config.closeName)});
        }
        const float legendRoom = crt ? 70 * u : 140 * u;
        const bool legend = type.Width(hint, look.small) + legendRoom + (crt ? 12 : 44) * u <= pw;
        type.Draw(px + (crt ? 6 : 22) * u, middle(fy, h, look.small), look.small, look.dim,
                  type.Fit(hint, look.small, pw - (legend ? legendRoom : 0) - (crt ? 12 : 44) * u));
        if (!legend) {
        } else if (crt) {
            const std::string legend = "* " + Capitals(Tr("changed"));
            type.Draw(px + pw - 6 * u - type.Width(legend, look.small), middle(fy, h, look.small), look.small, look.dim, legend);
        } else {
            const std::string legend = Tr("changed");
            const float lw = type.Width(legend, look.small);
            const float lx = px + pw - 22 * u - lw;
            FillRound(canvas, lx - 15 * u, std::round(fy + h / 2 - 4 * u), 8 * u, 8 * u, 4 * u, look.changed);
            type.Draw(lx, middle(fy, h, look.small), look.small, look.dim, legend);
        }
    }
}

} // namespace recomp
