// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The runtime's access to the state of Ymir's sound CPU (a friend of MC68EC000 through the Ymir patch)
#pragma once

#include <ymir/hw/m68k/m68k.hpp>

#include <cstdint>

namespace satrecomp {

struct M68KAccess {
    using CPU = ymir::m68k::MC68EC000;
    static uint32_t PC(CPU &c) {
        return c.PC;
    }
    static uint32_t D(CPU &c, unsigned n) {
        return c.regs.D[n];
    }
    static uint32_t A(CPU &c, unsigned n) {
        return c.regs.A[n];
    }
    static uint32_t OtherSP(CPU &c) {
        return c.SP_swap;
    }
    static uint16_t SR(CPU &c) {
        return c.SR.u16;
    }
    static uint8_t InterruptLevel(CPU &c) {
        return c.m_externalInterruptLevel;
    }
};

} // namespace satrecomp
