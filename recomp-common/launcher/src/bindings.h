// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
//
// Which key and which gamepad button does what, for each player: the launcher's Controllers page writes the
// player's choices to settings.ini and the game reads them. Both include this file (it needs only SDL3) and
// hand it the port's table of actions; nothing else here is about a system.
//
//   [keyboard1]         a key by SDL's name for it ("Up", "A", "Return"), or "none"
//   up = Up
//   [gamepad1]          a button or a trigger by SDL's name ("a", "leftshoulder", "righttrigger"), or "none"
//   lp = x
//
// (and [keyboard2], [gamepad2] for player 2, and so on for a port with more players). What is not in the file
// is as the table's usual layout has it.
//
// Which gamepad is which player's (the Controllers page's Connected list):
//
//   [controllers]       a gamepad (PadIdentity) = the player it is (1 to 4), "none" (nobody's) or "auto"
//   <GUID> = 1           (SDL's GUID for the gamepad: 32 hex digits)
//   <GUID>-2 = 2         (the second one of that kind connected)
//
// A gamepad not listed, or "auto", goes to the first player who has none, in the order they were connected; one
// chosen for a player who is not connected leaves that player to the automatic ones. So with nothing chosen the
// first gamepad is player 1's, the second player 2's, and so on (as it always was), and a Steam Deck's own controls
// can be "none" or a later player, so two gamepads plugged into it play each other.
//
// A game reads the file once when it starts (LoadBindings) and from then on looks only at the table in
// memory: nothing is looked up by name while playing.
#pragma once

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace recomp {

inline constexpr int kBindPlayers = 2;    // how many players a table has unless it says
inline constexpr int kBindMaxPlayers = 4; // ... and the most it can say
inline constexpr int kBindMaxActions = 32;

// A gamepad's button, or one of its triggers, or nothing
struct PadInput {
    int button = -1; // SDL_GamepadButton
    int axis = -1;   // SDL_GamepadAxis (a trigger)
    bool operator==(const PadInput &other) const { return button == other.button && axis == other.axis; }
    bool operator!=(const PadInput &other) const { return !(*this == other); }
};

// Something that can be bound: its name in settings.ini, what the launcher calls it, and the usual layout
// (each player's key and gamepad input when nothing is chosen)
struct BindAction {
    const char *name;
    const char *title;
    SDL_Scancode key[kBindMaxPlayers]; // (players a table lists none for: no key, no gamepad input)
    PadInput pad[kBindMaxPlayers];
};

// A port's actions (at most kBindMaxActions) and how many players have them (1 to kBindMaxPlayers)
struct BindTable {
    const BindAction *actions = nullptr;
    int count = 0;
    int players = kBindPlayers;
};

struct Bindings {
    SDL_Scancode key[kBindMaxPlayers][kBindMaxActions];
    PadInput pad[kBindMaxPlayers][kBindMaxActions];
};

// The layout with nothing chosen
inline Bindings DefaultBindings(const BindTable &table) {
    Bindings b{};
    for (int player = 0; player < kBindMaxPlayers; player++) {
        for (int action = 0; action < kBindMaxActions; action++) {
            const bool has = action < table.count && player < table.players;
            b.key[player][action] = has ? table.actions[action].key[player] : SDL_SCANCODE_UNKNOWN;
            b.pad[player][action] = has ? table.actions[action].pad[player] : PadInput{};
        }
    }
    return b;
}

// --- as text, for settings.ini ---

inline std::string KeyText(SDL_Scancode key) {
    const char *name = key == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(key);
    return name && *name ? name : "none";
}
inline SDL_Scancode KeyFromText(const std::string &text) {
    return text == "none" ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(text.c_str());
}
inline std::string PadText(PadInput input) {
    const char *name = input.button >= 0 ? SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(input.button))
                       : input.axis >= 0 ? SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(input.axis))
                                         : nullptr;
    return name && *name ? name : "none";
}
inline PadInput PadFromText(const std::string &text) {
    PadInput input;
    if (text != "none") {
        input.button = SDL_GetGamepadButtonFromString(text.c_str());
        if (input.button < 0) {
            input.axis = SDL_GetGamepadAxisFromString(text.c_str());
            if (input.axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && input.axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                input.axis = -1;
            }
        }
    }
    return input;
}
inline std::string BindingSection(bool gamepad, int player) {
    return (gamepad ? "gamepad" : "keyboard") + std::to_string(player + 1);
}

// One line of settings.ini taken in: `section` is the heading it is under
inline void ApplyBinding(Bindings &b, const BindTable &table, const std::string &section, const std::string &name, const std::string &value) {
    for (int player = 0; player < table.players; player++) {
        for (int gamepad = 0; gamepad < 2; gamepad++) {
            if (section != BindingSection(gamepad, player)) {
                continue;
            }
            for (int action = 0; action < table.count; action++) {
                if (name == table.actions[action].name) {
                    if (gamepad) {
                        b.pad[player][action] = PadFromText(value);
                    } else if (value == "none" || KeyFromText(value) != SDL_SCANCODE_UNKNOWN) {
                        b.key[player][action] = KeyFromText(value);
                    }
                }
            }
        }
    }
}

// Each line of settings.ini, with the heading it is under (nothing, when there is no file)
template <typename Line> inline void ReadSettingsLines(const std::string &settingsPath, Line line_of) {
    // (the path is UTF-8: by its wide name on Windows, where a folder with an accent is not in the system's code page)
#ifdef __cpp_char8_t
    std::ifstream file(std::filesystem::path(std::u8string(settingsPath.begin(), settingsPath.end())));
#else
    std::ifstream file(std::filesystem::u8path(settingsPath));
#endif
    std::string line, section;
    auto trim = [](std::string text) {
        const size_t first = text.find_first_not_of(" \t\r"), last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    while (std::getline(file, line)) {
        line = trim(line);
        const size_t equals = line.find('=');
        if (!line.empty() && line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
        } else if (equals != std::string::npos) {
            line_of(section, trim(line.substr(0, equals)), trim(line.substr(equals + 1)));
        }
    }
}

// The player's choices from settings.ini (the usual layout where it says nothing, or there is no file)
inline Bindings LoadBindings(const std::string &settingsPath, const BindTable &table) {
    Bindings b = DefaultBindings(table);
    ReadSettingsLines(settingsPath, [&](const std::string &section, const std::string &name, const std::string &value) {
        ApplyBinding(b, table, section, name, value);
    });
    return b;
}

// Some player has this gamepad input bound (then it does not also do what it does otherwise while playing)
inline bool PadInputBound(const Bindings &b, const BindTable &table, PadInput input) {
    for (int player = 0; player < table.players; player++) {
        for (int action = 0; action < table.count; action++) {
            if (b.pad[player][action] == input) {
                return true;
            }
        }
    }
    return false;
}

// --- which gamepad is which player's ---

inline constexpr const char *kPadPlayersSection = "controllers";
inline constexpr int kPadAuto = -1;   // given to the first player without one
inline constexpr int kPadNobody = -2; // nobody's: it plays nothing

// A connected gamepad as settings.ini names it: its kind (SDL's GUID, the same each time it is plugged in), and,
// for the second of a kind connected and so on, its place among them ("-2"). (Two of the same gamepad are told apart
// only by the order they were connected in; under Steam every gamepad is a Steam one, in Steam's own order.)
inline std::string PadIdentity(SDL_JoystickID id) {
    int count = 0;
    SDL_JoystickID *all = SDL_GetGamepads(&count);
    const SDL_GUID guid = SDL_GetGamepadGUIDForID(id);
    int same = 0;
    for (int i = 0; all && i < count && all[i] != id; i++) {
        const SDL_GUID other = SDL_GetGamepadGUIDForID(all[i]);
        same += std::memcmp(other.data, guid.data, sizeof guid.data) == 0;
    }
    SDL_free(all);
    char text[33];
    SDL_GUIDToString(guid, text, sizeof text);
    return same == 0 ? std::string(text) : std::string(text) + "-" + std::to_string(same + 1);
}

// What [controllers] says: each gamepad named there, and the player it is (0 to 3), kPadAuto or kPadNobody
struct PadChoices {
    std::map<std::string, int> player;
    int of(const std::string &identity) const {
        auto it = player.find(identity);
        return it == player.end() ? kPadAuto : it->second;
    }
};

inline std::string PadChoiceText(int player) {
    return player == kPadNobody ? "none" : player >= 0 ? std::to_string(player + 1) : "auto";
}
inline int PadChoiceFromText(const std::string &text) {
    if (text == "none") {
        return kPadNobody;
    }
    const int number = std::atoi(text.c_str());
    return number >= 1 && number <= kBindMaxPlayers ? number - 1 : kPadAuto;
}

inline PadChoices LoadPadChoices(const std::string &settingsPath) {
    PadChoices choices;
    ReadSettingsLines(settingsPath, [&](const std::string &section, const std::string &name, const std::string &value) {
        if (section == kPadPlayersSection && !name.empty()) {
            choices.player[name] = PadChoiceFromText(value);
        }
    });
    return choices;
}

// Each player's gamepad (nobody's: -1) from the connected ones, in the order they were connected (`identities`,
// PadIdentity's): those chosen for a player are that player's (the first, if two were chosen for one), and the rest
// go in order to the players who have none. `players` is how many there are (at most kBindMaxPlayers). An empty
// identity is an empty seat (a game that keeps a let-go gamepad's place for it): it takes an automatic place, so the
// players after it keep theirs, and that player plays nothing until it is back.
inline void AssignPads(const PadChoices &choices, const std::vector<std::string> &identities, int players, int out[kBindMaxPlayers]) {
    for (int player = 0; player < kBindMaxPlayers; player++) {
        out[player] = -1;
    }
    players = players < kBindMaxPlayers ? players : kBindMaxPlayers;
    const int count = static_cast<int>(identities.size());
    std::vector<int> chosen(identities.size());
    for (int i = 0; i < count; i++) {
        int &player = chosen[static_cast<size_t>(i)];
        player = identities[static_cast<size_t>(i)].empty() ? kPadAuto : choices.of(identities[static_cast<size_t>(i)]);
        if (player >= 0 && player < players && out[player] < 0) {
            out[player] = i;
        } else if (player >= 0) {
            player = kPadAuto; // (its player has one, or the game has no such player)
        }
    }
    for (int i = 0, player = 0; i < count; i++) {
        if (chosen[static_cast<size_t>(i)] != kPadAuto) {
            continue;
        }
        while (player < players && out[player] >= 0) {
            player++;
        }
        if (player < players) {
            out[player] = i;
        }
    }
}

// The same for SDL's gamepads by id
inline void AssignPads(const PadChoices &choices, const SDL_JoystickID *ids, int count, int players, int out[kBindMaxPlayers]) {
    std::vector<std::string> identities;
    for (int i = 0; i < count; i++) {
        identities.push_back(PadIdentity(ids[i]));
    }
    AssignPads(choices, identities, players, out);
}

// The same for a game's open gamepads, in the order they were connected (nullptr: an empty seat): each player's (or
// nullptr). A game calls it when one is connected or let go, not every frame (it asks SDL for every gamepad's GUID).
inline void AssignPads(const PadChoices &choices, SDL_Gamepad *const *open, int count, int players, SDL_Gamepad *out[kBindMaxPlayers]) {
    std::vector<std::string> identities;
    for (int i = 0; i < count; i++) {
        identities.push_back(open[i] ? PadIdentity(SDL_GetGamepadID(open[i])) : std::string());
    }
    int index[kBindMaxPlayers];
    AssignPads(choices, identities, players, index);
    for (int player = 0; player < kBindMaxPlayers; player++) {
        out[player] = index[player] >= 0 ? open[index[player]] : nullptr;
    }
}

} // namespace recomp
