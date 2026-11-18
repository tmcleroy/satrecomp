// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// The country flags beside the players in the waiting room, and the country a player chooses to show with their name:
// every ISO country's flag (countries.h, the pictures in flags.png from flag-icons, MIT), compiled in.
#pragma once

#include <string>
#include <string_view>

struct SDL_Renderer;

namespace launcher::flags {

// The country's index in countries.h (its code, either case), or -1: not a country there
int find(std::string_view code);
// The code as countries.h has it (upper case), or "" for one not there
std::string known(std::string_view code);
// The country's flag at the cursor, `height` points tall (4:3), and an item of that size; false (nothing drawn) for a
// code not there or with no picture (an SDL before 3.4 reads no PNG)
bool draw(SDL_Renderer* renderer, std::string_view code, float height);

} // namespace launcher::flags
