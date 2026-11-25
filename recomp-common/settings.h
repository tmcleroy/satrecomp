// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
//
// The settings a port's launcher and the game itself share, and the rules both must agree on. Nothing here
// knows of a game, a system or SDL.
//
//   [features]
//   preset = pure | enhanced | custom
//   name = 0 | 1 | N        (N: the number of one of a feature's choices; no value: as the preset has it)
//   name_id = crt-lottes    (a feature with ids: its choice by name, and name = its number in the old order too)
//   [menu]
//   name = 1,3,4            (a kFeatureMenuChoices feature: the choices the game's menu offers; no value: all)
//   name_id = crt,xbrz      (... of a feature with ids, by name, and name = their old numbers too)
//
// The game prints its features (FeatureListText, for its --list-features flag); the launcher reads them
// (ParseFeatureList). Both read a value with FeatureValues and change one with SetFeature, which writes only
// the lines it must: everything else in the file stays as it was.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace recomp {

// A small INI file editor that keeps everything it does not touch (comments, sections and keys it was not
// asked about, their order).
class Ini {
public:
    explicit Ini(std::filesystem::path path);
    // (a path as text is UTF-8, as SDL's and the launcher's are: by its wide name on Windows, where a folder with an
    // accent in it is not in the system's code page)
    explicit Ini(const std::string &path);
    explicit Ini(const char *path) : Ini(std::string(path)) {}
    void load();
    bool save() const;
    // "" when the key is not there
    std::string get(const std::string &section, const std::string &key) const;
    void set(const std::string &section, const std::string &key, const std::string &value);
    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
    std::vector<std::string> lines_;
};

enum : unsigned {
    kFeatureEnhanced = 2,  // on in the "enhanced" preset
    kFeatureExclusive = 4, // only one of its category's exclusive features on at a time
    kFeatureSession = 8,   // for one run of the game only: never kept in the file, not shown by a launcher
    // (16, 32: a port's own)
    kFeatureOwn = 64,      // the game's own: a collection's launcher never sets it for another game (a record, say)
    // A choice of several whose list in the game's menu the player may make shorter (in the launcher: the favourites
    // of a long list): MenuChoices
    kFeatureMenuChoices = 128,
};

struct FeatureInfo {
    std::string name;     // as in the file
    std::string category; // the heading it is under
    std::string title;
    std::string description;
    unsigned flags = 0;
    // For a choice of several things rather than on or off: their names, the first meaning off
    std::vector<std::string> choices;
    // A choice's stable names, one for each choice (the first, off, "off"), when it is kept by name: in the file as
    // name_id = <id>, so the list can be put in another order without a saved choice changing its meaning. Empty: by
    // number, as ever. (Not the same count as the choices: by number.)
    std::vector<std::string> ids = {};
    // With ids: what the numbers of the plain name = N meant, the ids in that old order ("" for a number that names
    // none now). Read when there is no name_id (a file from before the ids) and written beside it, so an older build
    // that knows only the numbers reads the same choice. Empty: the numbers are the ids' own order.
    std::vector<std::string> numbered = {};
};

// The list as text, a feature a line: name, category, flags, title, description and (only when there are
// some) the choices with '|' between them, tab-separated; then, for a feature kept by name, its ids and its numbered
// the same way (a seventh and eighth field). Session features are left out. A reader that knows nothing of choices
// sees the five fields it expects; a list with no sixth field has no choices, one with no seventh no ids.
std::string FeatureListText(const std::vector<FeatureInfo> &features);
std::vector<FeatureInfo> ParseFeatureList(const std::string &text);
// A line of the list that is about the program, not a feature: "!note", a tab, and a few words the launcher shows by
// the game's name (a program built with none of its game's code translated, say, which plays slower). One line, no
// tabs. A reader that knows nothing of it skips it, as it skips any line of fewer than five fields.
std::string FeatureListNote(const std::string &note);
// ... and the note in a list ("" when it has none)
std::string ParseFeatureListNote(const std::string &text);

// A value that means off: 0, off, false, no
bool SettingOff(const std::string &value);

// "pure", "enhanced" or "custom" ("pure" when the file says nothing)
std::string Preset(const Ini &ini);

// Every feature's value, in the list's order: 0 off, 1 on, or the number of the choice. A feature the file
// gives no value has the preset's (on only in "enhanced", and only if it is kFeatureEnhanced); a session
// feature is 0; a choice the feature has not is 0; of a category's exclusive features only the first on is.
// A feature with ids: name_id's choice; with none (or an id this build does not know), name's number in the
// numbered order.
std::vector<int> FeatureValues(const Ini &ini, const std::vector<FeatureInfo> &features);

// Whether the file gives the feature a value of its own (chosen, or written down when the preset became custom), not
// the preset's
bool FeatureChosen(const Ini &ini, const FeatureInfo &feature);

// Gives one feature a value. The preset becomes "custom", and on the way there every feature that followed
// the preset is written down as it was, so nothing else changes; a value already in the file is never
// rewritten. Switching an exclusive feature on switches its category's other exclusive ones off. A session
// feature is not written. A feature with ids is written as name_id = <id> and name = <its old number> ("" when
// it has none), and the file is first made over as MigrateFeatures does. (The caller saves.)
void SetFeature(Ini &ini, const std::vector<FeatureInfo> &features, size_t index, int value);

// A file from before a feature had ids made over: its numbers written as names too (name_id, [menu] name_id),
// and for one that has the names, the numbers beside them made to agree (name_id wins). An id this build does not
// know (a newer build's) is left as it is. Whether anything changed. (The caller saves.)
bool MigrateFeatures(Ini &ini, const std::vector<FeatureInfo> &features);

// Of a kFeatureMenuChoices feature, whether the game's menu offers each of its choices (as [menu] says; every one when
// it says nothing, or the feature is not one; by name, name_id, for a feature with ids, or its old numbers when that
// has none). The first, off, always is.
std::vector<bool> MenuChoices(const Ini &ini, const FeatureInfo &feature);
// ... and setting them (every one offered: the key taken out; with ids, name_id and the old numbers). (The caller saves.)
void SetMenuChoices(Ini &ini, const FeatureInfo &feature, const std::vector<bool> &offered);

// Chooses a preset. "pure" and "enhanced" take every feature's own value out of the file (they follow the
// preset again); "custom" leaves them. (The caller saves.)
void SetPreset(Ini &ini, const std::vector<FeatureInfo> &features, const std::string &preset);

} // namespace recomp
