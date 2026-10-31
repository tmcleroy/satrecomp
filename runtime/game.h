// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// What a game's own sources (games/<id>/src) tell the port besides the routines they replace (lift.h):
// optional features the player can switch (cheats and the like), what to do after every frame, how its
// screens should be treated in widescreen, and its own state for save states.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "../recomp-common/settings.h"

namespace satrecomp {

class Archive; // (state.h)

// An optional feature. The launcher lists them (--list-features) and writes the player's choices to
// settings.ini, section [features]; the in-game menu shows each on the page of its category.
struct Feature {
    const char *name;        // as in settings.ini and --features
    const char *category;    // heading in the launcher
    const char *title;
    const char *description;
    unsigned flags = 0;      // kFeatureEnhanced, kFeatureExclusive, ...
    // For a choice of several things rather than on or off: their names, the first meaning off
    std::vector<const char *> choices = {};
};
enum : unsigned {
    kFeatureEnhanced = 2,  // on in the launcher's "Enhanced" preset
    kFeatureExclusive = 4, // only one of its category at a time
    // Its switch is on the launcher's first page (the Display card), not on the Features page: the launcher
    // says at every start whether it is on, by an argument of its own. The in-game menu still has it, and
    // changing it there changes the launcher's switch too (launcher.ini), or the launcher would turn it back.
    kFeatureFirstPage = 16,
    // A training aid (the readouts, the dummy...): it works only while a training session is on
    // (Game::inTraining); elsewhere it reads as off (FeatureOn, FeatureValue), though the menu and
    // settings.ini keep it
    kFeatureTraining = 32,
};

// Widescreen: part of a scroll screen that belongs at the picture's sides (video/renderer.h, WideSplit)
struct WideSplit {
    uint32_t layer, rowFrom, rowTo, leftEnd, rightStart;
};

struct Game {
    std::vector<Feature> features;
    std::vector<WideSplit> wideSplits;
    void (*frame)() = nullptr;                                     // after every frame the game ran
    void (*state)(Archive &ar) = nullptr;                          // the game sources' own state, for save states
    // A training session is on (the game's own idea of one): the training aids (kFeatureTraining) work only
    // then. Not given: they work everywhere.
    bool (*inTraining)() = nullptr;
    // Not to run ahead now (the port's run-ahead): a moment whose workings a save state does not wholly
    // hold, so that frames run and taken back would change it. Not given: run-ahead whenever it is on.
    bool (*noRunAhead)() = nullptr;
};
// Filled in by RegisterGame (lift.h)
Game &TheGame();

// The features as the launcher and the settings rules know them (recomp-common/settings.h): --list-features
// prints recomp::FeatureListText of this
std::vector<recomp::FeatureInfo> FeatureList();

// The player's choices: from settings.ini ([features]: preset = pure | enhanced | custom, then name = 0 | 1),
// then `list` (comma-separated names, each on; "-name": off). Call after RegisterGame.
void LoadFeatures(const std::string &settingsPath, const std::string &list);
bool FeatureOn(const char *name);
// Changes one and writes settings.ini as the launcher would (preset = custom, every feature's state)
void SetFeature(const char *name, bool on);
// For a feature with choices: which is chosen (0: the first, which is off), and choosing. (FeatureOn: not 0.)
int FeatureValue(const char *name);
void SetFeatureValue(const char *name, int value);
// What the player has chosen for a feature, whether it works now or not (the menu shows this; FeatureValue
// gives 0 for a training aid outside a training session)
int FeatureSetting(const char *name);
// The game's sources mark features of theirs as training aids (kFeatureTraining), by name
void TrainingAids(std::initializer_list<const char *> names);
// Run-ahead (the port's "run_ahead" feature) is running frames that will be taken back: the game's sources
// skip what must happen once (writing a file, keeping what is not part of their save state)
bool RunningAhead();
void SetRunningAhead(bool on);
// Where a game's sources may write a file of their own for the player (a table to read outside the game):
// `name` in the folder settings.ini is in. Empty when the game was started without one.
std::string DataFilePath(const std::string &name);

// The readout: lines of text the game's sources want shown over the picture while it plays (a training
// display and the like). The port draws each as a small panel.
struct HudLine {
    float x, y;       // where, as parts of the picture's width and height (0-1)
    int align;        // -1: the text starts at x; 0: is centred on it; 1: ends at it
    std::string text;
    bool operator==(const HudLine &) const = default;
};
void SetHud(std::vector<HudLine> lines);
// A diagram beside the lines: circles on a dark panel (a view of the game's collision shapes, say). The
// panel is placed like a line; its size is in parts of the picture's height; the circles are in units of
// the diagram's own, unitsWide by unitsHigh, from the bottom left.
struct HudCircle {
    float x, y, radius;
    uint8_t r, g, b;
    bool filled;
    bool operator==(const HudCircle &) const = default;
};
struct HudDiagram {
    float x = 0, y = 0;
    int align = 0;
    float width = 0, height = 0; // (0: no diagram)
    float unitsWide = 1, unitsHigh = 1;
    std::vector<HudCircle> circles;
    bool operator==(const HudDiagram &) const = default;
};
void SetHudDiagram(HudDiagram diagram);
const HudDiagram &TheHudDiagram();
// Circles drawn straight over the picture (shapes of the game's laid over what they belong to): x and y as
// parts of the picture's width and height, the radius as a part of its height
void SetHudCircles(std::vector<HudCircle> circles);
const std::vector<HudCircle> &HudCircles();
const std::vector<HudLine> &Hud();
uint64_t HudSerial(); // (goes up when the lines change)

// The shape of the picture shown (its width for its height: 4:3, or wider in widescreen), for what is laid
// over the game's own picture
float PictureAspect();
void SetPictureAspect(float aspect);

// Work RAM as the game's sources see it: a pointer to the byte at a Saturn address (high work RAM
// 0x06000000, low 0x00200000), null elsewhere. Values there are big-endian.
uint8_t *Ram(uint32_t address);
inline uint16_t Ram16(uint32_t address) {
    const uint8_t *p = Ram(address);
    return p ? static_cast<uint16_t>((p[0] << 8) | p[1]) : 0;
}
inline uint32_t Ram32(uint32_t address) {
    const uint8_t *p = Ram(address);
    return p ? (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3] : 0;
}
inline void SetRam16(uint32_t address, uint16_t value) {
    if (uint8_t *p = Ram(address)) {
        p[0] = static_cast<uint8_t>(value >> 8);
        p[1] = static_cast<uint8_t>(value);
    }
}
inline void SetRam32(uint32_t address, uint32_t value) {
    if (uint8_t *p = Ram(address)) {
        p[0] = static_cast<uint8_t>(value >> 24);
        p[1] = static_cast<uint8_t>(value >> 16);
        p[2] = static_cast<uint8_t>(value >> 8);
        p[3] = static_cast<uint8_t>(value);
    }
}

} // namespace satrecomp
