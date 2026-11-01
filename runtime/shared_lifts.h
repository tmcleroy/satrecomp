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
// close; and its Fread, which reads a number of sectors from a file's read position.
extern const Routine kGfsOpen, kGfsStart, kGfsPoll, kGfsClose, kGfsFread;
// The CD library: play audio tracks, pause or stop, report the drive's state (runtime/music.h plays them)
extern const Routine kCdPlay, kCdSeek, kCdGetCurStat;
// The peripheral library's SMPC command routine: answers INTBACK (system status and both control pads)
// from the host's pads, without the chip
extern const Routine kPerSendCommand;
// Sega AM2's engine (Fighting Vipers, Fighters Megamix): load a whole file and wait; read both pads a frame
extern const Routine kAm2LoadFile, kAm2ReadPads;

// Registers `routine` at `address` (name: as it is listed, if not the routine's own)
void Register(const Routine &routine, uint32_t address, const char *name = nullptr);

// Save states: the files open through the library, what the system manager has been told and its clock
void State(Archive &ar);

} // namespace shared
} // namespace satrecomp
