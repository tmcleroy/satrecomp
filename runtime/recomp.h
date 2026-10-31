// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// satrecomp runtime: runs statically recompiled SH-2 code in place of the interpreter.
#pragma once

#include "backend.h"

#include <cstdio>
#include <string>
#include <vector>

namespace satrecomp {

// Work RAM was written without going through the CPU or the bus (a file loaded straight into memory): any
// translated code has to be compared with memory again
void CodeChanged();


enum class Mode {
    // Cycle exact: identical to the unmodified interpreter (`--cpu pure`)
    Recompiled,  // recompiled blocks; the interpreter runs the blocks the build doesn't have
    Interpreter, // the interpreter runs every block: the exact reference for Recompiled
    // Free running: blocks run to their end whatever the time slice, time is nominal cycles per instruction
    Native,      // satrecomp's own translation of the blocks (block_native.h)
    Free,        // the interpreter runs every block the same way: the exact reference for Native
};

struct Options {
    Mode mode = Mode::Recompiled;
    std::string recordPath; // block database to update with newly reached blocks
    bool stats = false;
    // Addresses where a CPU, when free running, pauses so that the other one gets time (games/<id>/game.cfg).
    // Blocks never run across them.
    std::vector<uint32_t> syncPoints;
};

void Attach(Machine &saturn, const Options &options);
void Finish();

// The sound CPU (m68k_recomp.cpp): recompiled blocks of the sound driver in place of its interpreter.
// recordPath: its block database to update (empty: don't).
void AttachM68K(Machine &saturn, const std::string &recordPath, bool stats);
void FinishM68K();

// Debugging aid: forget how often each block ran so far
void ResetRunCounts();

// Writes the opcode tables for tools/recomp.py and recomp68k.py: 2 x 65536 little-endian uint16 SH-2 opcode
// types (ordinary instructions, then delay slot instructions), 65536 bytes of nominal cycles per SH-2 opcode,
// then 65536 bytes of sound CPU opcode types.
bool DumpDecodeTable(const std::string &path);

// A save state was loaded: the CPUs are somewhere else now, so what the dispatcher keeps about where they
// were (functions stopped or waiting for a call to come back, loops known to be waiting) is forgotten
void StateLoaded();
// Run-ahead (port/main.cpp): the same, kept before frames that will be taken back and put back after them in
// place of forgetting it, so that the frames that follow run exactly as they would have without them
void KeepDispatcher();
void PutDispatcherBack();

} // namespace satrecomp

namespace satrecomp {
// (part of DumpDecodeTable: 65536 bytes, the sound CPU's opcode types)
bool DumpM68KDecodeTable(FILE *f);
} // namespace satrecomp
