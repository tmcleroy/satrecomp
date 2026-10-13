// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The sound CPU (MC68EC000): tables of recompiled blocks, the interface between generated code
// (tools/recomp68k.py) and the runtime (m68k_recomp.cpp).
#pragma once

#include <ymir/hw/m68k/m68k.hpp>

#include <cstddef>
#include <cstdint>

namespace satrecomp {

// A recompiled block: runs its instructions from the first, for up to `budget` cycles, and returns the cycles
// taken. It returns early when control leaves the block or an interrupt is taken, as the interpreter would.
using M68KBlockFn = uint64_t (*)(ymir::m68k::MC68EC000 &cpu, uint64_t budget);

// A run of instructions as it was executed once: the address of the first and, for each, its offset and
// opcode word. (Extension words are read when the instruction runs, so they are not part of what identifies
// the block.)
struct M68KInstr {
    uint16_t offset;
    uint16_t opcode;
};

struct M68KBlockDesc {
    uint32_t address;
    uint16_t numInstrs;
    const M68KInstr *instrs;
    M68KBlockFn fn;
};

struct M68KBlockTable {
    const M68KBlockDesc *blocks;
    size_t count;
};

// Defined by the generated m68k_index.cpp (or by no_m68k_blocks.cpp when nothing was generated)
extern const M68KBlockTable g_m68kBlockTables[];
extern const size_t g_m68kBlockTableCount;

inline constexpr unsigned kMaxM68KBlockInstrs = 32;

} // namespace satrecomp

// Generated code: one instruction, then leave unless execution goes on at the block's next instruction
// (NEXT: its offset) with time left. Every instruction is the interpreter's handler with a constant opcode;
// before each, an interrupt is taken if one is due, exactly as the interpreter does.
#define SM_BEGIN()                  \
    using namespace ymir::m68k;     \
    uint64_t cy = 0;                \
    const uint32_t base = c.PC - 4;

#define SM_OP(NEXT, ...)                                 \
    if (c.CheckInterrupt()) [[unlikely]] {               \
        return cy + 44;                                  \
    }                                                    \
    cy += c.__VA_ARGS__;                                 \
    if (cy >= budget || c.PC - 4 != base + (NEXT)) {     \
        return cy;                                       \
    }

#define SM_LAST(...)                                     \
    if (c.CheckInterrupt()) [[unlikely]] {               \
        return cy + 44;                                  \
    }                                                    \
    cy += c.__VA_ARGS__;                                 \
    return cy;
