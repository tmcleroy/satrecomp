// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Block tables: the interface between generated code (tools/recomp.py) and the runtime.
#pragma once

#include "backend.h"

#include <cstddef>
#include <cstdint>

namespace satrecomp {

// A recompiled block: runs instructions of one block on the given CPU, starting at instruction index `start`,
// adding to the CPU's executed cycle count. It returns as soon as the interpreter would not simply go on to the
// next instruction of the block: when the time slice is used up (`target` cycles executed), when control leaves
// the block, or when an interrupt is due.
using BlockFn = void (*)(SH2 &cpu, uint64_t target, unsigned start);

// A straight run of SH-2 instructions, identified by its address and its exact code bytes. The same address can
// hold different code at different times (programs loaded from disc over one another); each is its own block.
// Code translated by satrecomp itself (tools/sh2emit.py, runtime/block_native.h): a block, or a function of
// several blocks. It counts nominal cycles, and runs until control leaves it, an interrupt is pending or, at a
// backward branch, the time slice is over (`target` cycles). `start` is the block of the function to start
// at: 0, its entry, or the block it stopped at before (which it says in the CPU's m_recompResume) or was to
// come back to from a call.
using NativeFn = void (*)(SH2 &cpu, uint64_t target, unsigned start);

// A call made by translated code (the CPU's PC is the callee, PR the return address): runs the callee's
// translation right away, if there is one for the code that is there now. True if it came back to the return
// address `ret` and the caller, a function that has a block there (number `retBlock`), can go on with it;
// if not (the time slice ended inside, an interrupt came up, it went elsewhere), the caller returns, and is
// taken up again at that block when control does arrive at `ret`. retBlock < 0: the caller has no block there.
bool Call(SH2 &cpu, uint64_t target, int retBlock, uint32_t ret);

struct BlockDesc {
    uint32_t pc;         // address (0x06xxxxxx high work RAM, 0x002xxxxx low work RAM, 0x000xxxxx BIOS ROM)
    uint16_t numInstrs;  // code size is numInstrs * 2 bytes
    const uint8_t *code; // the instructions, as in Saturn memory (big-endian)
    BlockFn fn;          // cycle-exact translation (the interpreter's handlers)
    NativeFn native;     // satrecomp's own translation
};

// A native function made of several blocks joined by their branches (tools/recomp.py). Inside it a branch to
// another of its blocks is a goto and a call is a call (Call above); the CPU leaves it at returns and computed
// jumps, and at a backward branch once its time slice is over.
struct FunctionBlock {
    int32_t offset;      // from the function's entry address
    uint16_t size;       // bytes
    const uint8_t *code;
};

struct FunctionDesc {
    uint32_t pc;
    NativeFn fn;
    const FunctionBlock *blocks; // blocks[0] is the entry
    uint16_t numBlocks;
};

struct BlockTable {
    const BlockDesc *blocks;
    size_t count;
    const FunctionDesc *functions;
    size_t functionCount;
};

// Defined by the generated blocks_index.cpp (or by no_blocks.cpp when nothing was generated)
extern const BlockTable g_blockTables[];
extern const size_t g_blockTableCount;

// Longest block the recorder makes
inline constexpr unsigned kMaxBlockInstrs = 48;

} // namespace satrecomp
