// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
//
// The player's own shader presets (RetroArch's .slangp, drawn by a port through librashader): where they go, finding
// them, and which one the player chose. A port's Shader feature has a choice, "Your own", that draws through one of
// these; the port keeps its own built-in presets and its librashader code.
//
//   The shared folder: shaders/ in the launcher's own folder (beside launcher.ini: %APPDATA%\<launcher>\shaders\,
//   ~/Library/Application Support/<launcher>/shaders/, ~/.local/share/<launcher>/shaders/, or user/<launcher>/shaders/
//   in portable mode), one for every game. The launcher makes it and tells the games it starts in RECOMP_SHADERS; a
//   game started on its own finds it beside its own folder (the launcher's folder is a sibling of each game's). It is
//   the player's: an update never touches it (it is not in the install's folder, or in portable mode is in user/).
//   A game's own: shaders/ in the game's folder (beside its settings.ini), for that game alone. Its presets come
//   first, and one of the same name as a shared one is used in its place.
//
// Every .slangp in either folder or under it is found (a RetroArch shader pack goes in whole, in a folder of its own,
// with the passes it uses beside it), and is known by its name: its path in the folder, '/' between folders
// ("crt-pack/crt-newpixie.slangp"). The player's choice is kept by that name in each game's settings.ini:
//
//   [shader]
//   own = crt-pack/crt-newpixie.slangp     (none: the first found, the game's own first)
//
// The launcher and the in-game menu show the same list (Choices): "The first one found", then each preset's name, and
// a chosen one that is no longer there at the end, so a list built anew each time keeps the player's choice by name,
// not by its place. The words are in the player's language (i18n.h). Nothing here knows of SDL or librashader.
// Header only, C++17.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "folders.h"
#include "i18n.h"
#include "settings.h"

namespace recomp::shaders {

constexpr const char *kFolder = "shaders";
constexpr const char *kEnvironment = "RECOMP_SHADERS"; // (the shared folder, as the launcher gives it to the games)
constexpr const char *kSection = "shader";
constexpr const char *kKey = "own";
constexpr const char *kExtension = ".slangp";
constexpr size_t kMost = 4000;  // (presets listed at most: the whole of RetroArch's slang-shaders is about 1900)
constexpr const char *kFirstFound = "The first one found"; // (the lists' first choice: none chosen)
constexpr const char *kTitle = "Your own preset";          // (the launcher's row, and the in-game menu's)

// A path as UTF-8 text and back, in C++17 and C++20 alike (where u8string is of char8_t, and u8path is deprecated)
inline std::string Utf8(const std::filesystem::path &path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();
#endif
}
inline std::filesystem::path PathOf(const std::string &utf8) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}

// The shared folder in the launcher's own folder
inline std::filesystem::path LauncherFolder(const std::filesystem::path &launcher_folder) {
    return launcher_folder / kFolder;
}

// The launcher, starting: the shared folder made (if it can be), and the games it starts told of it. True if it is
// there.
inline bool Prepare(const std::filesystem::path &shared) {
    std::error_code error;
    std::filesystem::create_directories(shared, error);
    const std::string path = Utf8(shared);
#ifdef _WIN32
    _putenv_s(kEnvironment, path.c_str());
#else
    setenv(kEnvironment, path.c_str(), 1);
#endif
    return std::filesystem::is_directory(shared, error);
}

// The shared folder as a game finds it, from its own folder (where its settings.ini is): RECOMP_SHADERS (the
// launcher's), else beside the game's folder, in the launcher's folder (named `launcher_name`, as the launcher's
// LAUNCHER_GAME_NAME). Not made here.
inline std::filesystem::path SharedFolder(const std::filesystem::path &game_folder, const std::string &launcher_name) {
    if (const char *given = std::getenv(kEnvironment); given && *given) {
        return PathOf(given);
    }
    if (game_folder.empty()) {
        return {};
    }
    std::filesystem::path folder = game_folder.lexically_normal();
    if (!folder.has_filename() && folder.has_relative_path()) {
        folder = folder.parent_path(); // ("a/b/" is "a/b")
    }
    return folder.parent_path() / PathOf(FolderName(launcher_name)) / kFolder;
}

// A game's own folder of presets, in its folder
inline std::filesystem::path GameFolder(const std::filesystem::path &game_folder) {
    return game_folder.empty() ? std::filesystem::path() : game_folder / kFolder;
}

struct Preset {
    std::string name;            // its path in its folder, '/' between folders
    std::filesystem::path file;  // where it is
    bool game_own = false;       // in the game's own folder
};

namespace detail {
    inline std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
}

// Every .slangp in a folder and the folders under it (links to other folders not followed), by name, sorted
// (letters' case aside). None for a folder that isn't there.
inline std::vector<Preset> Scan(const std::filesystem::path &folder, bool game_own = false) {
    namespace fs = std::filesystem;
    std::vector<Preset> found;
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) {
        return found;
    }
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
    for (; !error && it != end && found.size() < kMost; it.increment(error)) {
        std::error_code ignored;
        if (it.depth() > 8) {
            it.disable_recursion_pending();
        }
        if (detail::Lower(Utf8(it->path().extension())) != kExtension || !it->is_regular_file(ignored)) {
            continue;
        }
        std::string name = Utf8(it->path().lexically_relative(folder));
#ifdef _WIN32
        std::replace(name.begin(), name.end(), '\\', '/');  // ('/' between folders everywhere)
#endif
        found.push_back({name, it->path(), game_own});
    }
    std::sort(found.begin(), found.end(), [](const Preset &a, const Preset &b) {
        const std::string la = detail::Lower(a.name), lb = detail::Lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    return found;
}

// A game's presets: its own folder's first, then the shared folder's that its own has none of the same name of
inline std::vector<Preset> Find(const std::filesystem::path &game_own, const std::filesystem::path &shared) {
    // (a launcher of one game keeps its things in the game's folder: the shared folder is then the game's own, and
    // its presets are the shared ones)
    std::error_code error;
    if (!game_own.empty() && std::filesystem::equivalent(game_own, shared, error)) {
        return Scan(shared);
    }
    std::vector<Preset> found = Scan(game_own, true);
    const size_t own = found.size();
    for (Preset &preset : Scan(shared)) {
        if (std::none_of(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(own), [&](const Preset &p) { return p.name == preset.name; })) {
            found.push_back(std::move(preset));
        }
    }
    return found;
}

// The player's choice, by name ("": none, the first found)
inline std::string Chosen(const Ini &ini) {
    return ini.get(kSection, kKey);
}
// ... and choosing one ("": none). (The caller saves.)
inline void Choose(Ini &ini, const std::string &name) {
    ini.set(kSection, kKey, name);
}

// The preset to draw with: the one chosen, or with none chosen the first found. nullptr if there is none, with why
// (for the player).
inline const Preset *Pick(const std::vector<Preset> &found, const std::string &chosen, std::string &why) {
    why.clear();
    if (chosen.empty()) {
        if (found.empty()) {
            why = i18n::Tr("no .slangp preset in the shaders folder");
        }
        return found.empty() ? nullptr : &found.front();
    }
    for (const Preset &preset : found) {
        if (preset.name == chosen) {
            return &preset;
        }
    }
    why = i18n::TrFormat("{} is not in the shaders folder", {chosen});
    return nullptr;
}

// A preset as the lists name it: its name without .slangp, and "(this game's)" after one in the game's own folder
inline std::string Label(const Preset &preset) {
    std::string label = preset.name;
    if (label.size() > std::char_traits<char>::length(kExtension) &&
        detail::Lower(label.substr(label.size() - std::char_traits<char>::length(kExtension))) == kExtension) {
        label.resize(label.size() - std::char_traits<char>::length(kExtension));
    }
    return preset.game_own ? i18n::TrFormat("{} (this game's)", {label}) : label;
}

// The list the launcher and the menu choose from: "The first one found" (none chosen), each preset found, and the one
// chosen at the end if it is not there any more ("<name> (not found)"), so it stays chosen until another is
struct Choices {
    std::vector<std::string> labels; // as shown (the first in English, as the launcher's and the menu's words are: Tr draws it)
    std::vector<std::string> names;  // as kept ("" for the first)
    // The place of a name in the list (0: none, or "")
    int IndexOf(const std::string &name) const {
        for (size_t i = 1; i < names.size(); i++) {
            if (names[i] == name) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }
    // ... and the name at a place ("" for the first, or one not in the list)
    std::string NameAt(int index) const {
        return index > 0 && index < static_cast<int>(names.size()) ? names[static_cast<size_t>(index)] : std::string();
    }
};

inline Choices MakeChoices(const std::vector<Preset> &found, const std::string &chosen) {
    Choices list;
    list.labels.push_back(kFirstFound);
    list.names.push_back("");
    for (const Preset &preset : found) {
        // (a name in both folders is found once: Find keeps the game's own)
        list.labels.push_back(Label(preset));
        list.names.push_back(preset.name);
    }
    if (!chosen.empty() && list.IndexOf(chosen) == 0) {
        list.labels.push_back(i18n::TrFormat("{} (not found)", {chosen}));
        list.names.push_back(chosen);
    }
    return list;
}

// The in-game menu's row of them, its description: where the folders are, in the player's language. (A host builds the
// row's choices anew, MakeChoices, each time it opens the menu, so a preset put in meanwhile is there.)
inline std::string MenuDescription(const std::filesystem::path &shared, const std::filesystem::path &game_own) {
#ifdef __ANDROID__
    // (no button opens it there, and no import yet: said as it is)
    return i18n::TrFormat("What Shader's Your own draws with, kept by its name: a .slangp preset in {} or in a folder there, with the "
                          "files it uses beside it. On Android that is the app's own storage: a computer connected over USB can usually "
                          "reach it, the phone's file manager usually can't, and there is no import yet. This game's own shaders "
                          "folder, {}, comes first. Choosing one chooses Your own.",
                          {Utf8(shared), Utf8(game_own)});
#else
    return i18n::TrFormat("What Shader's Your own draws with, kept by its name: a .slangp preset in {} (one folder for every game: the "
                          "launcher's Open shaders folder opens it) or in a folder there, with the files it uses beside it. This game's "
                          "own shaders folder, {}, comes first. Choosing one chooses Your own.",
                          {Utf8(shared), Utf8(game_own)});
#endif
}

}  // namespace recomp::shaders
