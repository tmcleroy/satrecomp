// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Routines that several games carry byte for byte, replaced by the same C++ in each.
//
// Sega's libraries for the Saturn (the file system, the CD drive's commands, the peripheral library) were
// linked into many games, and games made on one engine share its routines too (Fighting Vipers and Fighters
// Megamix, both Sega AM2's, share their file loader and pad reader). Such a routine sits at another address
// in each game, and the work RAM it uses differs; the code itself is the same. The replacements here take
// every address they need from the routine's own code (its constant pool) or from where the library keeps
// its data next to its code, so a game only says where each routine is:
//
//     satrecomp::shared::Register(satrecomp::shared::kGfsOpen, 0x06044D78);
//
// The code at that address must hash the same as the routine studied (runtime/lift.h); if it does not, the
// game's own code runs.
#pragma once

#include "lift.h"

#include <initializer_list>

namespace satrecomp {
class Archive;

namespace shared {

struct Routine {
    const char *name;
    uint32_t size;  // bytes of code checked
    uint64_t hash;  // their LiftHash
    bool (*fn)(Guest &guest);
};

// The file system library (GFS): open a file by name, start a read into memory, ask how it is getting on,
// close. (Not its Fread: a replacement that read at once was tried and taken out. The game waits in Fread
// with its interrupts running while the drive reads, and Sega AM2's games load their sound program and
// samples through it: done at once, their sound effects stopped after the first in a fight. The game's own
// Fread reads through the CD drive, satrecomp/machine/cd.cpp.)
extern const Routine kGfsOpen, kGfsStart, kGfsPoll, kGfsClose;
// The CD library: play audio tracks, pause or stop, report the drive's state (runtime/music.h plays them)
extern const Routine kCdPlay, kCdSeek, kCdGetCurStat;
// The peripheral library's SMPC command routine: answers INTBACK (system status and both control pads)
// from the host's pads, without the chip
extern const Routine kPerSendCommand;
// Sega AM2's engine (Fighting Vipers, Fighters Megamix): load a whole file and wait; read both pads a frame
extern const Routine kAm2LoadFile, kAm2ReadPads;

// Sega AM2's engine in widescreen: the routines that list a stage's panels (the cages, fences and walls of
// Fighting Vipers and Fighters Megamix, on each CPU) take a panel only if all four corners are less than 224
// dots from the picture's middle, 48 past the 4:3 picture's sides, so in a wider picture the panels at the
// sides were left out. Each routine loads the limit from a word next to its code (mov.w @(disp,PC),r1, then
// neg r1,r2 straight after or in the delay slot of the branch after). `loads` are those mov.w instructions;
// this sets each limit out by as much as the picture is wider and a margin more (at 4:3 it stays the game's). Called after
// every frame the game ran, since code may be loaded later and a state loaded may have been saved with
// another picture's limit. Code that isn't such a routine is left alone.
void WidenAm2PanelLimits(std::initializer_list<uint32_t> loads);

// Registers `routine` at `address` (name: as it is listed, if not the routine's own)
void Register(const Routine &routine, uint32_t address, const char *name = nullptr);

// Save states: the files open through the library, what the system manager has been told and its clock
void State(Archive &ar);

} // namespace shared
} // namespace satrecomp
