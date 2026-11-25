// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// A stand-in for a game program, for the launcher's example: it lists some features (--list-features) and,
// started, says what it was started with, what the settings file makes of the features, and which of the player's own
// shader presets Filter's Your own would draw with (own_shaders.h).
#include "own_shaders.h"
#include "settings.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    const std::vector<recomp::FeatureInfo> features = {
        {"skip_intro", "Game", "Skip the notices", "Straight to the title.", recomp::kFeatureEnhanced, {}},
        {"endless_time", "Cheats", "Endless time", "The clock stands still.", 0, {}},
        {"health", "Cheats", "Health", "Whose health is kept full.", 0, {"Off", "Player 1", "Both"}},
        {"easy", "Difficulty", "Easy", "Fewer and slower.", recomp::kFeatureExclusive, {}},
        {"hard", "Difficulty", "Hard", "More and faster.", recomp::kFeatureExclusive, {}},
        {"filter", "Picture", "Filter", "How the picture is drawn.", recomp::kFeatureMenuChoices, {"Off", "Soft", "Scanlines", "CRT", "Smooth edges", "Your own"}},
    };
    const char *settings = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list-features")) {
            fputs(recomp::FeatureListText(features).c_str(), stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--settings") && i + 1 < argc) {
            settings = argv[i + 1];
        }
        printf("argument: %s\n", argv[i]);
    }
    recomp::Ini ini(settings);
    ini.load();
    const std::vector<int> values = recomp::FeatureValues(ini, features);
    for (size_t i = 0; i < features.size(); i++) {
        printf("%s = %d\n", features[i].name.c_str(), values[i]);
    }
    // (the launcher's folder is this game's: a launcher of one game)
    const std::filesystem::path folder = std::filesystem::u8path(settings).parent_path();
    std::string why;
    const std::vector<recomp::shaders::Preset> found =
        recomp::shaders::Find(recomp::shaders::GameFolder(folder), recomp::shaders::SharedFolder(folder, "Example Game"));
    const recomp::shaders::Preset *preset = recomp::shaders::Pick(found, recomp::shaders::Chosen(ini), why);
    printf("own shader: %s\n", preset ? preset->file.u8string().c_str() : why.c_str());
    return 0;
}
