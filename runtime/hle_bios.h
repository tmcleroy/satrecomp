// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// A native replacement for the Saturn BIOS: the port starts the game from its disc without a BIOS image.
//
// What a game needs from the BIOS once it runs is small: the interrupt entry and exit code, a table of
// system services (set a handler, change the interrupt mask, change the clock, semaphores), the start-up
// of the slave SH-2 and the backup memory library. Here each of these is a C++ routine. The tables the BIOS
// keeps in low work RAM hold addresses in the (otherwise empty) ROM area; when a CPU arrives at one, the
// dispatcher runs the routine instead of code.
#pragma once

#include "backend.h"

#include <string>

#include <cstdint>
#include <vector>

namespace satrecomp {
class Archive;
}

namespace satrecomp::hle {

// What is read from the disc to start it
struct BootImage {
    std::vector<uint8_t> ip;        // the disc's system area (initial program), loaded at 0x06002000
    std::vector<uint8_t> firstRead; // the first file of the disc
    uint32_t firstReadAddress = 0;  // where it is loaded (from the disc header)
};

// Sets the machine up to start `image` without a BIOS. Call before the hard reset that starts it.
void Install(Machine &saturn, BootImage image);

bool Active();

// Save states (state.h): what the native BIOS is in the middle of
void Serialize(Archive &ar);

#ifdef SATRECOMP_OWN_MACHINE
// The save memory's image file (read now, written after every change)
void OpenBackup(const std::string &path);
#endif

// Runs the native routine at an address in the ROM area. Returns false if there is none.
bool Run(SH2 &cpu, uint32_t romOffset);

} // namespace satrecomp::hle
