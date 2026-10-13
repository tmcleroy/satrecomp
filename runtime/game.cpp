// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "game.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace satrecomp {

extern uint8_t *g_ramHigh; // (recomp.cpp)
extern uint8_t *g_ramLow;

Game &TheGame() {
    static Game game;
    return game;
}

uint8_t *Ram(uint32_t address) {
    if ((address & 0xC6000000u) == 0x06000000u) {
        return g_ramHigh ? g_ramHigh + (address & 0xFFFFFu) : nullptr;
    }
    if ((address & 0xC7F00000u) == 0x00200000u) {
        return g_ramLow ? g_ramLow + (address & 0xFFFFFu) : nullptr;
    }
    return nullptr;
}

namespace {
    std::map<std::string, bool> g_on;
    std::string g_settingsPath;

    std::string Trim(const std::string &s) {
        const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
} // namespace

std::vector<recomp::FeatureInfo> FeatureList() {
    std::vector<recomp::FeatureInfo> list;
    for (const Feature &feature : TheGame().features) {
        list.push_back({feature.name, feature.category, feature.title, feature.description, feature.flags, {}});
    }
    return list;
}

std::string DataFilePath(const std::string &name) {
    if (g_settingsPath.empty()) {
        return {};
    }
    const size_t slash = g_settingsPath.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : g_settingsPath.substr(0, slash + 1)) + name;
}

void LoadFeatures(const std::string &settingsPath, const std::string &list) {
    g_settingsPath = settingsPath;
    g_on.clear();
    // (the rules the launcher follows too: recomp-common/settings.h)
    recomp::Ini settings(settingsPath);
    settings.load();
    const std::vector<int> values = recomp::FeatureValues(settings, FeatureList());
    for (size_t i = 0; i < values.size(); i++) {
        g_on[TheGame().features[i].name] = values[i] != 0;
    }
    std::stringstream names(list);
    std::string name;
    while (std::getline(names, name, ',')) {
        name = Trim(name);
        if (!name.empty()) {
            const bool off = name[0] == '-';
            g_on[off ? name.substr(1) : name] = !off;
        }
    }
}

bool FeatureOn(const char *name) {
    const auto it = g_on.find(name);
    return it != g_on.end() && it->second;
}

void SetFeature(const char *name, bool on) {
    g_on[name] = on;
    // (one of a category's exclusive features at a time, as in the launcher)
    for (const Feature &changed : TheGame().features) {
        if (on && (changed.flags & kFeatureExclusive) && !strcmp(changed.name, name)) {
            for (const Feature &other : TheGame().features) {
                if ((other.flags & kFeatureExclusive) && !strcmp(other.category, changed.category) && strcmp(other.name, name)) {
                    g_on[other.name] = false;
                }
            }
        }
    }
    if (g_settingsPath.empty()) {
        return;
    }
    // The file as it is but for this feature (and, coming from a preset, the others written down as they
    // were): as the launcher writes it
    const std::vector<recomp::FeatureInfo> features = FeatureList();
    for (size_t i = 0; i < features.size(); i++) {
        if (features[i].name == name) {
            recomp::Ini settings(g_settingsPath);
            settings.load();
            recomp::SetFeature(settings, features, i, on ? 1 : 0);
            settings.save();
        }
    }
}

namespace {
    std::vector<HudLine> g_hud;
    uint64_t g_hudSerial = 0;
} // namespace

void SetHud(std::vector<HudLine> lines) {
    if (lines != g_hud) {
        g_hud = std::move(lines);
        g_hudSerial++;
        // (debugging aid, SATRECOMP_HUD_LOG=1: the readout's lines whenever they change, for tests with no window)
        static const bool log = getenv("SATRECOMP_HUD_LOG") != nullptr;
        if (log) {
            std::string all;
            for (const HudLine &line : g_hud) {
                all += (all.empty() ? "" : " | ") + line.text;
            }
            fprintf(stderr, "hud: %s\n", all.c_str());
        }
    }
}
namespace {
    HudDiagram g_diagram;
} // namespace
void SetHudDiagram(HudDiagram diagram) {
    if (diagram != g_diagram) {
        g_diagram = std::move(diagram);
        g_hudSerial++;
    }
}
const HudDiagram &TheHudDiagram() {
    return g_diagram;
}
namespace {
    std::vector<HudCircle> g_circles;
} // namespace
namespace {
    float g_aspect = 4.0f / 3.0f;
} // namespace
float PictureAspect() {
    return g_aspect;
}
void SetPictureAspect(float aspect) {
    g_aspect = aspect;
}
void SetHudCircles(std::vector<HudCircle> circles) {
    if (circles != g_circles) {
        g_circles = std::move(circles);
        g_hudSerial++;
    }
}
const std::vector<HudCircle> &HudCircles() {
    return g_circles;
}
const std::vector<HudLine> &Hud() {
    return g_hud;
}
uint64_t HudSerial() {
    return g_hudSerial;
}

} // namespace satrecomp
