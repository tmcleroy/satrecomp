// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The runtime's access to the SH-2's state.
#pragma once

#include "backend.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace satrecomp {


// The runtime's window into the CPU (a friend of SH2 through the Ymir patch)
struct Access {
    // Registers, for native replacements of BIOS routines (hle_bios.cpp)
    static std::array<uint32_t, 16> &R(SH2 &c) {
        return c.R;
    }
    static void SetPC(SH2 &c, uint32_t pc) {
        c.PC = pc;
    }
    static void SetPR(SH2 &c, uint32_t pr) {
        c.PR = pr;
    }
    static uint32_t GBR(SH2 &c) {
        return c.GBR;
    }
    static void SetGBR(SH2 &c, uint32_t v) {
        c.GBR = v;
    }
    static uint32_t VBR(SH2 &c) {
        return c.VBR;
    }
    static void SetVBR(SH2 &c, uint32_t v) {
        c.VBR = v;
    }
    static void ClearMAC(SH2 &c) {
        c.MAC.u64 = 0;
    }
    // Writes SR as ldc does: an interrupt may become due
    static void SetSR(SH2 &c, uint32_t sr) {
        c.SR.u32 = sr & 0x3F3u;
        c.m_intrFlags.pending = c.INTC.pending.level > c.SR.ILevel;
    }
    static void SetT(SH2 &c, bool t) {
        c.SR.T = t;
    }
    // Guest memory as the CPU sees it, including its on-chip registers (defined in hle_bios.cpp)
    static uint8_t Read8(SH2 &c, uint32_t address);
    static uint16_t Read16(SH2 &c, uint32_t address);
    static uint32_t Read32(SH2 &c, uint32_t address);
    static void Write8(SH2 &c, uint32_t address, uint8_t value);
    static void Write16(SH2 &c, uint32_t address, uint16_t value);
    static void Write32(SH2 &c, uint32_t address, uint32_t value);

    static uint32_t PC(SH2 &c) {
        return c.PC;
    }
    static uint32_t SR(SH2 &c) {
        return c.SR.u32;
    }
    static uint32_t PR(SH2 &c) {
        return c.PR;
    }
    static uint32_t R15(SH2 &c) {
        return c.R[15];
    }
    // Whether the CPU is known to be only waiting (see the dispatcher)
    static void SetWaiting(SH2 &c, bool waiting) {
        c.m_recompWaiting = waiting;
    }
    // Everything a block's outcome depends on besides memory: for telling that a CPU's state has come round
    struct Registers {
        std::array<uint32_t, 16> R;
        uint32_t PC, PR, SR, GBR, VBR;
        uint64_t MAC;
        bool operator==(const Registers &) const = default;
    };
    static Registers GetRegisters(SH2 &c) {
        return {c.R, c.PC, c.PR, c.SR.u32, c.GBR, c.VBR, c.MAC.u64};
    }
    // An interrupt is waiting to be serviced (now, or after one more instruction if it is being held off)
    // Where a function stopped (block_native.h): 0, or the number of the block + 1
    static uint32_t Resume(SH2 &c) {
        return c.m_recompResume;
    }
    static void SetResume(SH2 &c, uint32_t value) {
        c.m_recompResume = value;
    }
    // Lets the other CPU run now, before this one's time slice is over
    static void Yield(SH2 &c) {
        c.m_recompYield = true;
    }
    static bool InterruptRaised(SH2 &c) {
        return c.m_intrFlags.pending;
    }
    static void AddCycles(SH2 &c, uint64_t cycles) {
        c.m_cyclesExecuted += cycles;
    }
    static uint64_t Cycles(SH2 &c) {
        return c.m_cyclesExecuted;
    }
    static bool InDelaySlot(SH2 &c) {
        return c.m_delaySlot;
    }
    static void PrintRegs(SH2 &c) {
        fprintf(stderr, "PC=%08X PR=%08X SR=%03X GBR=%08X R:", c.PC, c.PR, c.SR.u32, c.GBR);
        for (uint32_t r : c.R) {
            fprintf(stderr, " %08X", r);
        }
        fprintf(stderr, "\n");
    }
    static void Trace(SH2 &c, FILE *f, uint64_t index, uint32_t blockPC, uint64_t &hash, bool full) {
        hash = (hash ^ c.PC) * 0x100000001b3ull;
        hash = (hash ^ c.m_cyclesExecuted) * 0x100000001b3ull;
        hash = (hash ^ c.SR.u32) * 0x100000001b3ull;
        for (uint32_t r : c.R) {
            hash = (hash ^ r) * 0x100000001b3ull;
        }
        if (full) {
            fprintf(f, "%llu %c blk=%08X pc=%08X cyc=%llu sr=%03X pr=%08X intr=%d%d ds=%d", (unsigned long long)index,
                    c.IsMaster() ? 'M' : 'S', blockPC, c.PC, (unsigned long long)c.m_cyclesExecuted, c.SR.u32, c.PR,
                    c.m_intrFlags.pending, c.m_intrFlags.allow, c.m_delaySlot);
            for (uint32_t r : c.R) {
                fprintf(f, " %08X", r);
            }
            fprintf(f, "\n");
        }
    }
};

} // namespace satrecomp
