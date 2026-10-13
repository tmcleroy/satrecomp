// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// CD music played by the port itself: audio tracks read straight from the disc image, a sample at a time,
// into the sound chip's CD audio input (where the game's own volume settings and fades apply to it).
//
// A game's lifted routines (lift.h) start and stop it in place of the commands the original sends to the CD
// drive.
#pragma once

#include <cstdint>
#include <functional>

namespace satrecomp {
class Archive;
}

namespace satrecomp::music {

// The disc's audio tracks, provided by the port: how many sectors a track has (0: no such audio track), and
// one sector of it (2352 bytes: 588 stereo samples, 16 bits little-endian)
struct Disc {
    std::function<uint32_t(int track)> sectors;
    std::function<bool(int track, uint32_t sector, uint8_t *out)> read;
};
void SetDisc(Disc disc);

bool IsAudioTrack(int track);
// Plays tracks `first` to `last` from the start, and after that `repeats` times more (15: until stopped)
void Play(int first, int last, int repeats);
void Pause();
void Resume();
void Stop();

enum class State { Stopped, Paused, Playing };
State GetState();
int CurrentTrack();
uint32_t CurrentSector();

// Save states (state.h): what is playing and where
void Serialize(Archive &ar);

// The next sample. False (and silence) when nothing is playing.
bool Next(int16_t &left, int16_t &right);

} // namespace satrecomp::music
