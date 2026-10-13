// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The readout over the picture: what the game's sources set (runtime/game.h: lines of text as small panels,
// a diagram on a panel, circles over the picture) and a note of the port's own.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace port {

// A line of the port's own, at the top (paused, the speed, a reset point set); empty: none
void SetHudNote(const std::string &text);
// (goes up when what is shown changes: the game's part or the note)
uint64_t HudShownSerial();
// Is there anything to show?
bool HudShows();

// Draws it over a picture (32-bit pixels, red in the low byte, alpha in the high one; over a picture that is
// all zeros this gives the readout alone, to be blended over the game's picture by its alpha)
void DrawHud(uint32_t *pixels, int width, int height);

} // namespace port
