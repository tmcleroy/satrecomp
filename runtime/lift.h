// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Lifted routines: game code replaced by hand-written C++.
//
// A game's own sources (games/<id>/src) register routines by guest address. When a CPU, free running, arrives
// at such an address and the code there is the routine that was studied (same hash), the C++ function runs in
// its place. It works on the guest's registers and memory through `Guest`, and leaves with Return() as the
// original would with rts. If it returns false nothing has happened and the original code runs.
//
// The interpreter reference (--cpu free) runs lifted routines too, so `make verify` keeps comparing the
// translated code around them. SATRECOMP_NO_LIFTS=1 runs the original code everywhere.
#pragma once

#include "backend.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace satrecomp {

// The guest CPU as a lifted routine sees it
class Guest {
public:
    explicit Guest(SH2 &cpu)
        : m_cpu(cpu) {}

    uint32_t &R(unsigned n);
    uint32_t PR() const;
    // Where the CPU is: in a lifted routine, before it has done anything, the routine's own address
    uint32_t PC() const;
    uint32_t GBR() const;
    uint8_t Read8(uint32_t address);
    uint16_t Read16(uint32_t address);
    uint32_t Read32(uint32_t address);
    void Write8(uint32_t address, uint8_t value);
    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);
    // A NUL-terminated string in guest memory
    std::string String(uint32_t address, size_t max = 256);
    // Copies host data into guest memory (work RAM, or hardware memory such as video and sound RAM)
    void CopyIn(uint32_t address, const uint8_t *data, size_t size);
    // Calls a routine of the game's (its own code, run by the interpreter until it comes back), as jsr does:
    // arguments and results are in the registers. It runs as the rest of the game does (translated code,
    // rewrites), but at once: no interrupt is taken and no time passes for the rest of the machine, so a
    // routine that waits for something cannot be called. For rewrites (Lift::rewrite) that call routines
    // not rewritten yet; on satrecomp's own machine only. The address may be inside a routine: a rewrite
    // of a routine's first part calls the rest of it, with the registers as the game's code has them there
    // (those the rest reads: tools/liveout.py ">address").
    void Call(uint32_t address);
    // Leaves the routine as rts does; `cycles` is the time it is said to have taken
    void Return(uint64_t cycles = 100);

private:
    SH2 &m_cpu;
};

struct Lift {
    const char *name;
    uint32_t address;         // where the routine starts
    uint32_t size;            // bytes of code checked (from the start)
    uint64_t hash;            // LiftHash of those bytes (tools/lifthash.py)
    bool (*fn)(Guest &guest); // false: do nothing, run the original
    // A rewrite: readable code that does exactly what the routine does (the same memory, registers and
    // writes to hardware, given the same before). SATRECOMP_LIFT_CHECK=1 runs both for every call and
    // compares (docs/READABLE.md).
    bool rewrite = false;
    // The game's routines are mostly hand-written assembly, and a caller may go on to use whatever a routine
    // happens to leave in a register. So the check compares every register and the T flag, except those
    // named here (bit n: rn; bit 16: T): the ones no caller reads before writing (tools/liveout.py works
    // that out from the code). The multiply registers (MACH, MACL) are not compared.
    uint32_t free = 0;
};

// FNV-1a, 64 bits
uint64_t LiftHash(const uint8_t *data, size_t size);

// Called by the game's sources before the machine starts
void RegisterLifts(const Lift *lifts, size_t count);

// Host services for lifted routines, provided by the port.
// Reads a whole file of the game disc by name; false if there is no such file.
using DiscFileReader = std::function<bool(const std::string &name, std::vector<uint8_t> &data)>;
void SetDiscFileReader(DiscFileReader reader);
bool ReadDiscFile(const std::string &name, std::vector<uint8_t> &data);

// Reads one sector (2048 bytes) of the disc's data track by its frame address; false past its end.
using DiscSectorReader = std::function<bool(uint32_t frameAddress, uint8_t *out)>;
void SetDiscSectorReader(DiscSectorReader reader);
bool ReadDiscSector(uint32_t frameAddress, uint8_t *out);
// The buttons a player holds right now (Saturn control pad layout, 1 = pressed; player 0 or 1).
using PadReader = std::function<uint16_t(int player)>;
void SetPadReader(PadReader reader);
uint16_t ReadPad(int player);

// Defined by the game (games/<id>/src); registers its lifts. The default (runtime/no_game.cpp) does nothing.
void RegisterGame();

} // namespace satrecomp
