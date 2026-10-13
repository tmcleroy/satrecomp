// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Building blocks of generated code. A generated file includes Ymir's sh2.cpp with SATRECOMP_OPS_ONLY (which
// gives it the instruction handlers), then this header, then defines its blocks as members of BlockSet<N>.
//
// Every instruction is a call to the interpreter's own handler with the opcode as a constant, so the compiler
// specialises it (register numbers, immediates and displacements fold away) while behaviour and cycle counts
// stay those of the interpreter. After every instruction the block makes the checks the interpreter's run loop
// makes, so a recompiled block stops at exactly the instruction where the interpreter would stop: the port
// stays cycle for cycle identical to the interpreter. A block can be entered at any of its instructions
// (`start`), which is how execution resumes after a time slice ended in the middle of it.
#pragma once

#include "blocks.h"

#ifdef SATRECOMP_OWN_MACHINE
// (the interpreter's handlers belong to the reference machine: no such blocks in this build)
#define SR_FN(F) nullptr
#else
#define SR_FN(F) F


#include <bit>

// True when the interpreter would service an interrupt before the next instruction
#define SR_INTR_PENDING() (std::bit_cast<uint16>(c.m_intrFlags) == SH2::kIntrFlagsPendingAllowed)

#define SR_BLOCK_BEGIN()                        \
    SATRECOMP_USING_TYPES                       \
    const uint32 pc0 = c.PC - start * 2u;       \
    switch (start) {                            \
    default: return;

#define SR_BLOCK_END() }

// Entry point at instruction index I
#define SR_AT(I) case I:

// One instruction: the interpreter's handler, with the opcode as a constant
#define SR_EXEC(...)               \
    c.m_intrFlags.allow = true;    \
    c.m_cyclesExecuted += c.__VA_ARGS__;

// After an instruction that only works on registers (it always continues with the next one and cannot make an
// interrupt due): leave only if the time slice is used up
#define SR_NEXT()                           \
    if (c.m_cyclesExecuted >= target) {     \
        return;                             \
    }

// After any other instruction: also leave unless execution simply continues at instruction index NEXT
#define SR_NEXT_CHECKED(NEXT)                                                                  \
    if (c.m_cyclesExecuted >= target || c.PC != pc0 + (NEXT) * 2u || SR_INTR_PENDING()) {     \
        return;                                                                                \
    }

// After a branch that has a delay slot. If the time slice ends here the interpreter runs the slot instruction.
#define SR_SLOT_FOLLOWS() SR_NEXT()

// bt/s, bf/s: true if the branch is taken (the next instruction runs as its delay slot)
#define SR_BRANCH_TAKEN() (c.m_delaySlot)

#endif
