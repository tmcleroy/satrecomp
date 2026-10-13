// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "m68k.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace satrecomp::sound {

namespace {
    // Sizes: 0 a byte, 1 a word, 2 a long word
    constexpr uint32_t kMask[3] = {0xFF, 0xFFFF, 0xFFFFFFFF};
    constexpr uint32_t kSign[3] = {0x80, 0x8000, 0x80000000};
    constexpr uint32_t kBytes[3] = {1, 2, 4};

    int32_t Extend(uint32_t value, unsigned size) {
        return size == 0 ? static_cast<int8_t>(value) : size == 1 ? static_cast<int16_t>(value) : static_cast<int32_t>(value);
    }
    // What working out an operand's address and reading it costs: [long word][mode], mode 7 by register after
    constexpr uint8_t kOperandCycles[2][12] = {{0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4}, {0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8}};
    unsigned ModeIndex(unsigned mode, unsigned reg) {
        return mode < 7 ? mode : 7 + reg;
    }
} // namespace

uint16_t M68K::SR() const {
    return static_cast<uint16_t>(flagC | (flagV << 1) | (flagZ << 2) | (flagN << 3) | (flagX << 4) | (interruptMask << 8) |
                                 (supervisor << 13) | (trace << 15));
}

void M68K::SetSR(uint16_t value) {
    flagC = value & 1;
    flagV = value & 2;
    flagZ = value & 4;
    flagN = value & 8;
    flagX = value & 16;
    interruptMask = (value >> 8) & 7;
    trace = value & 0x8000;
    const bool super = value & 0x2000;
    if (super != supervisor) {
        std::swap(a[7], otherSP);
        supervisor = super;
    }
}

void M68K::Reset(bool hard) {
    if (hard) {
        for (int i = 0; i < 8; i++) {
            d[i] = a[i] = 0;
        }
        m_interruptLevel = 0;
    }
    a[7] = Read(0, 2);
    otherSP = 0;
    pc = Read(4, 2);
    flagC = flagV = flagZ = flagN = flagX = false;
    supervisor = true;
    trace = false;
    interruptMask = 7;
    stopped = false;
}

// --- memory ---

uint32_t M68K::Read(uint32_t address, unsigned size) {
    switch (size) {
    case 0: return m_bus.Read8(address & 0xFFFFFF);
    case 1: return m_bus.Read16(address & 0xFFFFFE);
    default: {
        const uint32_t high = m_bus.Read16(address & 0xFFFFFE);
        return (high << 16) | m_bus.Read16((address + 2) & 0xFFFFFE);
    }
    }
}
void M68K::Write(uint32_t address, unsigned size, uint32_t value) {
    switch (size) {
    case 0: m_bus.Write8(address & 0xFFFFFF, static_cast<uint8_t>(value)); break;
    case 1: m_bus.Write16(address & 0xFFFFFE, static_cast<uint16_t>(value)); break;
    default:
        m_bus.Write16(address & 0xFFFFFE, static_cast<uint16_t>(value >> 16));
        m_bus.Write16((address + 2) & 0xFFFFFE, static_cast<uint16_t>(value));
        break;
    }
}
uint16_t M68K::Fetch() {
    const uint16_t word = m_bus.Read16(pc & 0xFFFFFE);
    pc += 2;
    return word;
}
uint32_t M68K::Fetch32() {
    const uint32_t high = Fetch();
    return (high << 16) | Fetch();
}
void M68K::Push32(uint32_t value) {
    a[7] -= 4;
    Write(a[7], 2, value);
}
void M68K::Push16(uint16_t value) {
    a[7] -= 2;
    Write(a[7], 1, value);
}
uint32_t M68K::Pop32() {
    const uint32_t value = Read(a[7], 2);
    a[7] += 4;
    return value;
}
uint16_t M68K::Pop16() {
    const uint16_t value = static_cast<uint16_t>(Read(a[7], 1));
    a[7] += 2;
    return value;
}

// --- operands ---

// Where an instruction's operand is, from its mode and register fields (reading the extension words and
// stepping the address register where the mode says so)
M68K::Operand M68K::Locate(unsigned mode, unsigned reg, unsigned size) {
    using Kind = Operand::Kind;
    auto indexed = [&](uint32_t base) {
        const uint16_t ext = Fetch();
        const uint32_t index = (ext & 0x8000) ? a[(ext >> 12) & 7] : d[(ext >> 12) & 7];
        return base + static_cast<int8_t>(ext) + ((ext & 0x0800) ? index : static_cast<uint32_t>(static_cast<int16_t>(index)));
    };
    switch (mode) {
    case 0: return {Kind::Data, reg};
    case 1: return {Kind::Address, reg};
    case 2: return {Kind::Memory, a[reg]};
    case 3: {
        const uint32_t at = a[reg];
        a[reg] += (size == 0 && reg == 7) ? 2 : kBytes[size]; // (the stack pointer stays even)
        return {Kind::Memory, at};
    }
    case 4:
        a[reg] -= (size == 0 && reg == 7) ? 2 : kBytes[size];
        return {Kind::Memory, a[reg]};
    case 5: return {Kind::Memory, a[reg] + static_cast<int16_t>(Fetch())};
    case 6: return {Kind::Memory, indexed(a[reg])};
    default:
        switch (reg) {
        case 0: return {Kind::Memory, static_cast<uint32_t>(static_cast<int16_t>(Fetch()))};
        case 1: return {Kind::Memory, Fetch32()};
        case 2: {
            const uint32_t base = pc;
            return {Kind::Memory, base + static_cast<int16_t>(Fetch())};
        }
        case 3: {
            const uint32_t base = pc;
            return {Kind::Memory, indexed(base)};
        }
        default: return {Kind::Immediate, size == 2 ? Fetch32() : (Fetch() & kMask[size])};
        }
    }
}
uint32_t M68K::Load(const Operand &operand, unsigned size) {
    switch (operand.kind) {
    case Operand::Kind::Data: return d[operand.at] & kMask[size];
    case Operand::Kind::Address: return a[operand.at] & kMask[size];
    case Operand::Kind::Memory: return Read(operand.at, size);
    default: return operand.at;
    }
}
void M68K::Store(const Operand &operand, unsigned size, uint32_t value) {
    switch (operand.kind) {
    case Operand::Kind::Data: d[operand.at] = (d[operand.at] & ~kMask[size]) | (value & kMask[size]); break;
    case Operand::Kind::Address: a[operand.at] = value; break;
    case Operand::Kind::Memory: Write(operand.at, size, value); break;
    default: break;
    }
}

// --- arithmetic and flags ---

bool M68K::Condition(unsigned cc) const {
    switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return !flagC && !flagZ;
    case 3: return flagC || flagZ;
    case 4: return !flagC;
    case 5: return flagC;
    case 6: return !flagZ;
    case 7: return flagZ;
    case 8: return !flagV;
    case 9: return flagV;
    case 10: return !flagN;
    case 11: return flagN;
    case 12: return flagN == flagV;
    case 13: return flagN != flagV;
    case 14: return !flagZ && flagN == flagV;
    default: return flagZ || flagN != flagV;
    }
}
uint32_t M68K::Add(uint32_t x, uint32_t y, unsigned size, bool withX, bool keepZ) {
    const uint64_t sum = static_cast<uint64_t>(x & kMask[size]) + (y & kMask[size]) + (withX && flagX);
    const uint32_t result = static_cast<uint32_t>(sum) & kMask[size];
    flagC = flagX = (sum >> (kBytes[size] * 8)) & 1;
    flagV = (~(x ^ y) & (x ^ result) & kSign[size]) != 0;
    flagN = result & kSign[size];
    flagZ = keepZ ? (flagZ && result == 0) : result == 0;
    return result;
}
// x - y
uint32_t M68K::Sub(uint32_t x, uint32_t y, unsigned size, bool withX, bool keepZ, bool setX) {
    const uint64_t difference = static_cast<uint64_t>(x & kMask[size]) - (y & kMask[size]) - (withX && flagX);
    const uint32_t result = static_cast<uint32_t>(difference) & kMask[size];
    flagC = (difference >> (kBytes[size] * 8)) & 1;
    if (setX) {
        flagX = flagC;
    }
    flagV = ((x ^ y) & (x ^ result) & kSign[size]) != 0;
    flagN = result & kSign[size];
    flagZ = keepZ ? (flagZ && result == 0) : result == 0;
    return result;
}
void M68K::Logic(uint32_t result, unsigned size) {
    flagN = result & kSign[size];
    flagZ = (result & kMask[size]) == 0;
    flagV = flagC = false;
}
// kind: 0 arithmetic shift, 1 logical shift, 2 rotate through X, 3 rotate
uint32_t M68K::Shift(unsigned kind, bool left, uint32_t value, unsigned count, unsigned size) {
    const uint32_t sign = kSign[size];
    value &= kMask[size];
    flagV = false;
    flagC = kind == 2 ? flagX : false;
    for (unsigned i = 0; i < count; i++) {
        const bool out = left ? (value & sign) : (value & 1);
        uint32_t in = 0;
        switch (kind) {
        case 0: in = left ? 0 : (value & sign); break;
        case 1: in = 0; break;
        case 2: in = flagX ? (left ? 1 : sign) : 0; break;
        default: in = out ? (left ? 1 : sign) : 0; break;
        }
        const uint32_t before = value;
        value = left ? ((value << 1) & kMask[size]) | in : (value >> 1) | in;
        flagC = out;
        if (kind != 3) {
            flagX = out;
        }
        if (kind == 0 && left && ((before ^ value) & sign)) {
            flagV = true; // (the sign changed at some point)
        }
    }
    flagN = value & sign;
    flagZ = value == 0;
    return value;
}

// --- exceptions ---

void M68K::Exception(unsigned vector, uint32_t returnAddress, int level) {
    const uint16_t before = SR();
    if (!supervisor) {
        std::swap(a[7], otherSP);
        supervisor = true;
    }
    trace = false;
    if (level >= 0) {
        interruptMask = static_cast<uint8_t>(level);
    }
    Push32(returnAddress);
    Push16(before);
    pc = Read(vector * 4, 2);
}

void M68K::Illegal(uint16_t op, uint32_t address) {
    static unsigned reports = 8;
    if (reports) {
        reports--;
        fprintf(stderr, "sound CPU: instruction %04X at %06X is not one this interpreter runs\n", op, address);
    }
    Exception((op >> 12) == 0xA ? 10 : (op >> 12) == 0xF ? 11 : 4, address, -1);
    m_cycles += 34;
}

uint32_t M68K::Step() {
    if (m_interruptLevel == 7 || m_interruptLevel > interruptMask) {
        stopped = false;
        Exception(24 + m_interruptLevel, pc, m_interruptLevel); // (the chip asks for the autovector)
        return 44;
    }
    if (stopped) {
        return 4;
    }
    m_cycles = 0;
    Execute(Fetch());
    return m_cycles;
}

void M68K::Execute(uint16_t op) {
    using Kind = Operand::Kind;
    const uint32_t at = pc - 2; // the instruction's address
    const unsigned mode = (op >> 3) & 7, reg = op & 7, reg2 = (op >> 9) & 7;
    const unsigned size = (op >> 6) & 3; // (where the instruction has a size field)
    const unsigned modeIndex = ModeIndex(mode, reg);
    auto operandCycles = [&](unsigned sz) { return kOperandCycles[sz == 2][modeIndex]; };
    const bool isRegister = mode < 2;

    switch (op >> 12) {
    case 0x0: {
        if (op & 0x0100) {
            if (mode == 1) { // MOVEP: every other byte of memory to or from a data register
                const uint32_t address = a[reg] + static_cast<int16_t>(Fetch());
                const unsigned bytes = (op & 0x0040) ? 4 : 2;
                if (op & 0x0080) {
                    for (unsigned i = 0; i < bytes; i++) {
                        Write(address + i * 2, 0, d[reg2] >> ((bytes - 1 - i) * 8));
                    }
                } else {
                    uint32_t value = 0;
                    for (unsigned i = 0; i < bytes; i++) {
                        value = (value << 8) | Read(address + i * 2, 0);
                    }
                    d[reg2] = bytes == 4 ? value : (d[reg2] & 0xFFFF0000) | value;
                }
                m_cycles += bytes == 4 ? 24 : 16;
                return;
            }
        } else if (reg2 != 4 && reg2 != 7 && (op & 0x3F) == 0x3C && size < 2) {
            // ORI, ANDI, EORI to CCR (a byte) or SR (a word)
            const uint16_t value = Fetch();
            uint16_t sr = SR();
            const uint16_t affected = size == 0 ? 0x00FF : 0xFFFF;
            switch (reg2) {
            case 0: sr |= value & affected; break;
            case 1: sr &= value | ~affected; break;
            case 5: sr ^= value & affected; break;
            default: Illegal(op, at); return;
            }
            SetSR(size == 0 ? (SR() & 0xFF00) | (sr & 0x1F) : sr & 0xA71F);
            m_cycles += 20;
            return;
        }
        if ((op & 0x0100) || reg2 == 4) {
            // BTST, BCHG, BCLR, BSET: a bit of a data register (32 bits) or of a byte of memory
            const uint32_t number = (op & 0x0100) ? d[reg2] : Fetch();
            const unsigned kind = (op >> 6) & 3;
            const bool dynamic = op & 0x0100;
            if (mode == 0) {
                const uint32_t bit = 1u << (number & 31);
                flagZ = !(d[reg] & bit);
                switch (kind) {
                case 1: d[reg] ^= bit; break;
                case 2: d[reg] &= ~bit; break;
                case 3: d[reg] |= bit; break;
                default: break;
                }
                static constexpr uint8_t kCycles[4] = {6, 8, 10, 8};
                m_cycles += kCycles[kind] + (dynamic ? 0 : 4);
            } else {
                const Operand operand = Locate(mode, reg, 0);
                uint32_t value = Load(operand, 0);
                const uint32_t bit = 1u << (number & 7);
                flagZ = !(value & bit);
                switch (kind) {
                case 1: value ^= bit; break;
                case 2: value &= ~bit; break;
                case 3: value |= bit; break;
                default: break;
                }
                if (kind != 0) {
                    Store(operand, 0, value);
                }
                m_cycles += operandCycles(0) + (kind == 0 ? 4 : 8) + (dynamic ? 0 : 4);
            }
            return;
        }
        // ORI, ANDI, SUBI, ADDI, EORI, CMPI
        if (size == 3) {
            Illegal(op, at);
            return;
        }
        const uint32_t value = size == 2 ? Fetch32() : (Fetch() & kMask[size]);
        const Operand operand = Locate(mode, reg, size);
        const uint32_t target = Load(operand, size);
        uint32_t result = 0;
        switch (reg2) {
        case 0: result = target | value; Logic(result, size); break;
        case 1: result = target & value; Logic(result, size); break;
        case 2: result = Sub(target, value, size, false, false); break;
        case 3: result = Add(target, value, size, false, false); break;
        case 5: result = target ^ value; Logic(result, size); break;
        case 6: Sub(target, value, size, false, false, false); break;
        default: Illegal(op, at); return;
        }
        if (reg2 != 6) {
            Store(operand, size, result);
            m_cycles += mode == 0 ? (size == 2 ? 16 : 8) : operandCycles(size) + (size == 2 ? 20 : 12);
        } else {
            m_cycles += mode == 0 ? (size == 2 ? 14 : 8) : operandCycles(size) + (size == 2 ? 12 : 8);
        }
        return;
    }
    case 0x1:
    case 0x2:
    case 0x3: { // MOVE, MOVEA
        const unsigned sz = (op >> 12) == 1 ? 0 : (op >> 12) == 3 ? 1 : 2;
        const Operand source = Locate(mode, reg, sz);
        const uint32_t value = Load(source, sz);
        const unsigned dstMode = (op >> 6) & 7;
        m_cycles += 4 + operandCycles(sz);
        if (dstMode == 1) {
            a[reg2] = static_cast<uint32_t>(Extend(value, sz));
            return;
        }
        const Operand target = Locate(dstMode, reg2, sz);
        Store(target, sz, value);
        Logic(value, sz);
        m_cycles += kOperandCycles[sz == 2][ModeIndex(dstMode, reg2)];
        return;
    }
    case 0x4: {
        if (op == 0x4E71) { // NOP
            m_cycles += 4;
            return;
        }
        if (op == 0x4E75) { // RTS
            pc = Pop32();
            m_cycles += 16;
            return;
        }
        if (op == 0x4E73) { // RTE
            const uint16_t sr = Pop16();
            pc = Pop32();
            SetSR(sr & 0xA71F);
            m_cycles += 20;
            return;
        }
        if (op == 0x4E77) { // RTR
            const uint16_t ccr = Pop16();
            pc = Pop32();
            SetSR((SR() & 0xFF00) | (ccr & 0x1F));
            m_cycles += 20;
            return;
        }
        if (op == 0x4E70) { // RESET
            m_cycles += 132;
            return;
        }
        if (op == 0x4E72) { // STOP
            SetSR(Fetch() & 0xA71F);
            stopped = true;
            m_cycles += 4;
            return;
        }
        if (op == 0x4E76) { // TRAPV
            if (flagV) {
                Exception(7, pc, -1);
                m_cycles += 34;
            } else {
                m_cycles += 4;
            }
            return;
        }
        if ((op & 0xFFF0) == 0x4E40) { // TRAP
            Exception(32 + (op & 15), pc, -1);
            m_cycles += 38;
            return;
        }
        if ((op & 0xFFF8) == 0x4E50) { // LINK
            Push32(a[reg]);
            a[reg] = a[7];
            a[7] += static_cast<int16_t>(Fetch());
            m_cycles += 16;
            return;
        }
        if ((op & 0xFFF8) == 0x4E58) { // UNLK
            a[7] = a[reg];
            a[reg] = Pop32();
            m_cycles += 12;
            return;
        }
        if ((op & 0xFFF0) == 0x4E60) { // MOVE to and from USP
            if (op & 8) {
                a[reg] = otherSP;
            } else {
                otherSP = a[reg];
            }
            m_cycles += 4;
            return;
        }
        if ((op & 0xFF80) == 0x4E80) { // JSR, JMP
            static constexpr uint8_t kJump[12] = {0, 0, 8, 0, 0, 10, 14, 10, 12, 10, 14, 0};
            const Operand target = Locate(mode, reg, 2);
            if (!(op & 0x0040)) {
                Push32(pc);
                m_cycles += 8;
            }
            pc = target.at;
            m_cycles += kJump[modeIndex];
            return;
        }
        if ((op & 0xF1C0) == 0x41C0) { // LEA
            static constexpr uint8_t kLea[12] = {0, 0, 4, 0, 0, 8, 12, 8, 12, 8, 12, 0};
            a[reg2] = Locate(mode, reg, 2).at;
            m_cycles += kLea[modeIndex];
            return;
        }
        if ((op & 0xF1C0) == 0x4180) { // CHK
            const int16_t bound = static_cast<int16_t>(Load(Locate(mode, reg, 1), 1));
            const int16_t value = static_cast<int16_t>(d[reg2]);
            m_cycles += 10 + operandCycles(1);
            if (value < 0 || value > bound) {
                flagN = value < 0;
                Exception(6, pc, -1);
                m_cycles += 30;
            }
            return;
        }
        switch (op & 0xFF00) {
        case 0x4000: // NEGX, MOVE from SR
        case 0x4200: // CLR
        case 0x4400: // NEG, MOVE to CCR
        case 0x4600: // NOT, MOVE to SR
            if (size == 3) {
                if ((op & 0xFF00) == 0x4000) {
                    const Operand target = Locate(mode, reg, 1);
                    Store(target, 1, SR());
                    m_cycles += mode == 0 ? 6 : 8 + operandCycles(1);
                } else if ((op & 0xFF00) == 0x4200) {
                    Illegal(op, at);
                } else {
                    const uint16_t value = static_cast<uint16_t>(Load(Locate(mode, reg, 1), 1));
                    SetSR((op & 0xFF00) == 0x4400 ? (SR() & 0xFF00) | (value & 0x1F) : value & 0xA71F);
                    m_cycles += 12 + operandCycles(1);
                }
                return;
            } else {
                const Operand target = Locate(mode, reg, size);
                const uint32_t value = (op & 0xFF00) == 0x4200 ? 0 : Load(target, size);
                uint32_t result = 0;
                switch (op & 0xFF00) {
                case 0x4000: result = Sub(0, value, size, true, true); break;
                case 0x4200: Logic(0, size); break;
                case 0x4400: result = Sub(0, value, size, false, false); break;
                default: result = ~value & kMask[size]; Logic(result, size); break;
                }
                Store(target, size, result);
                m_cycles += isRegister ? (size == 2 ? 6 : 4) : operandCycles(size) + (size == 2 ? 12 : 8);
                return;
            }
        case 0x4800:
            if (size == 0) { // NBCD
                const Operand target = Locate(mode, reg, 0);
                const uint32_t value = Load(target, 0);
                int low = -static_cast<int>(value & 15) - flagX;
                int result = -static_cast<int>(value) - flagX;
                if (low < 0) {
                    result -= 6;
                }
                flagC = flagX = result < 0;
                if (result < 0) {
                    result -= 0x60;
                }
                flagZ = flagZ && (result & 0xFF) == 0;
                flagN = result & 0x80;
                Store(target, 0, static_cast<uint32_t>(result) & 0xFF);
                m_cycles += mode == 0 ? 6 : 8 + operandCycles(0);
                return;
            }
            if (size == 1) {
                if (mode == 0) { // SWAP
                    d[reg] = (d[reg] << 16) | (d[reg] >> 16);
                    Logic(d[reg], 2);
                    m_cycles += 4;
                } else { // PEA
                    static constexpr uint8_t kPea[12] = {0, 0, 12, 0, 0, 16, 20, 16, 20, 16, 20, 0};
                    Push32(Locate(mode, reg, 2).at);
                    m_cycles += kPea[modeIndex];
                }
                return;
            }
            if (mode == 0) { // EXT
                if (size == 2) {
                    d[reg] = (d[reg] & 0xFFFF0000) | (static_cast<uint32_t>(static_cast<int8_t>(d[reg])) & 0xFFFF);
                    Logic(d[reg], 1);
                } else {
                    d[reg] = static_cast<uint32_t>(static_cast<int16_t>(d[reg]));
                    Logic(d[reg], 2);
                }
                m_cycles += 4;
                return;
            }
            [[fallthrough]];
        case 0x4C00: { // MOVEM
            if (size < 2) {
                Illegal(op, at);
                return;
            }
            const unsigned sz = size == 3 ? 2 : 1;
            const uint16_t list = Fetch();
            const bool toRegisters = op & 0x0400;
            const unsigned count = static_cast<unsigned>(std::popcount(list));
            if (!toRegisters && mode == 4) {
                // (to memory going down: the list is the other way round, A7 first)
                uint32_t address = a[reg];
                for (int i = 0; i < 16; i++) {
                    if (list & (1u << i)) {
                        address -= kBytes[sz];
                        const unsigned r = 15 - i;
                        Write(address, sz, r < 8 ? d[r] : a[r - 8]);
                    }
                }
                a[reg] = address;
                m_cycles += 8 + count * (sz == 2 ? 8 : 4);
                return;
            }
            uint32_t address = mode == 3 ? a[reg] : Locate(mode, reg, sz).at;
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) {
                    continue;
                }
                if (toRegisters) {
                    const uint32_t value = static_cast<uint32_t>(Extend(Read(address, sz), sz));
                    (i < 8 ? d[i] : a[i - 8]) = value;
                } else {
                    Write(address, sz, i < 8 ? d[i] : a[i - 8]);
                }
                address += kBytes[sz];
            }
            if (mode == 3) {
                a[reg] = address;
            }
            static constexpr uint8_t kToMemory[12] = {0, 0, 8, 0, 8, 12, 14, 12, 16, 0, 0, 0};
            static constexpr uint8_t kToRegisters[12] = {0, 0, 16, 12, 0, 20, 22, 20, 24, 20, 22, 0};
            m_cycles += (toRegisters ? kToRegisters : kToMemory)[modeIndex] + count * (sz == 2 ? 8 : 4);
            return;
        }
        case 0x4A00:
            if (size == 3) { // TAS
                const Operand target = Locate(mode, reg, 0);
                const uint32_t value = Load(target, 0);
                Logic(value, 0);
                Store(target, 0, value | 0x80);
                m_cycles += mode == 0 ? 4 : 10 + operandCycles(0);
            } else { // TST
                Logic(Load(Locate(mode, reg, size), size), size);
                m_cycles += 4 + operandCycles(size);
            }
            return;
        default: break;
        }
        Illegal(op, at);
        return;
    }
    case 0x5: {
        if (size == 3) {
            const unsigned cc = (op >> 8) & 15;
            if (mode == 1) { // DBcc: count down and branch until the condition holds or the count runs out
                const uint32_t base = pc;
                const int16_t displacement = static_cast<int16_t>(Fetch());
                if (Condition(cc)) {
                    m_cycles += 12;
                } else {
                    const uint16_t count = static_cast<uint16_t>(d[reg] - 1);
                    d[reg] = (d[reg] & 0xFFFF0000) | count;
                    if (count != 0xFFFF) {
                        pc = base + displacement;
                        m_cycles += 10;
                    } else {
                        m_cycles += 14;
                    }
                }
            } else { // Scc
                const bool set = Condition(cc);
                Store(Locate(mode, reg, 0), 0, set ? 0xFF : 0);
                m_cycles += mode == 0 ? (set ? 6 : 4) : 8 + operandCycles(0);
            }
            return;
        }
        // ADDQ, SUBQ
        const uint32_t value = reg2 ? reg2 : 8;
        if (mode == 1) {
            a[reg] += (op & 0x0100) ? 0u - value : value;
            m_cycles += 8;
            return;
        }
        const Operand target = Locate(mode, reg, size);
        const uint32_t before = Load(target, size);
        Store(target, size, (op & 0x0100) ? Sub(before, value, size, false, false) : Add(before, value, size, false, false));
        m_cycles += mode == 0 ? (size == 2 ? 8 : 4) : operandCycles(size) + (size == 2 || !(op & 0x0100) ? 12 : 8);
        return;
    }
    case 0x6: { // Bcc, BRA, BSR
        const uint32_t base = pc;
        const unsigned cc = (op >> 8) & 15;
        int32_t displacement = static_cast<int8_t>(op);
        const bool wide = displacement == 0;
        if (wide) {
            displacement = static_cast<int16_t>(Fetch());
        }
        if (cc == 1) {
            Push32(pc);
            pc = base + displacement;
            m_cycles += 18;
        } else if (Condition(cc)) {
            pc = base + displacement;
            m_cycles += 10;
        } else {
            m_cycles += wide ? 12 : 8;
        }
        return;
    }
    case 0x7: // MOVEQ
        d[reg2] = static_cast<uint32_t>(static_cast<int8_t>(op));
        Logic(d[reg2], 2);
        m_cycles += 4;
        return;
    case 0x8:
    case 0xC: {
        const bool isAnd = (op >> 12) == 0xC;
        if (size == 3) { // DIVU, DIVS / MULU, MULS
            const uint32_t source = Load(Locate(mode, reg, 1), 1);
            const bool isSigned = op & 0x0100;
            if (isAnd) {
                if (isSigned) {
                    d[reg2] = static_cast<uint32_t>(static_cast<int16_t>(d[reg2]) * static_cast<int16_t>(source));
                    m_cycles += 38 + 2 * std::popcount(static_cast<uint16_t>(source ^ (source << 1)));
                } else {
                    d[reg2] = (d[reg2] & 0xFFFF) * source;
                    m_cycles += 38 + 2 * std::popcount(source);
                }
                Logic(d[reg2], 2);
                m_cycles += operandCycles(1);
                return;
            }
            if (source == 0) {
                Exception(5, pc, -1);
                flagN = flagZ = flagV = flagC = false;
                m_cycles += 42;
                return;
            }
            flagZ = flagC = false;
            if (isSigned) {
                const int32_t dividend = static_cast<int32_t>(d[reg2]);
                const int16_t divisor = static_cast<int16_t>(source);
                const int64_t quotient = static_cast<int64_t>(dividend) / divisor;
                const int64_t remainder = static_cast<int64_t>(dividend) % divisor;
                const uint32_t magnitude = static_cast<uint32_t>(dividend < 0 ? -static_cast<int64_t>(dividend) : dividend);
                const uint32_t limit = static_cast<uint32_t>(std::abs(static_cast<int32_t>(divisor))) << 16;
                if (magnitude >= limit && divisor != -0x8000) {
                    flagN = flagV = true;
                } else {
                    flagZ = quotient == 0;
                    flagV = quotient != static_cast<int16_t>(quotient);
                    if (flagV) {
                        flagN = true;
                    } else {
                        d[reg2] = static_cast<uint16_t>(quotient) | (static_cast<uint32_t>(static_cast<uint16_t>(remainder)) << 16);
                        flagN = quotient & 0x8000;
                    }
                }
                m_cycles += 158;
            } else {
                const uint32_t quotient = d[reg2] / source, remainder = d[reg2] % source;
                flagZ = quotient == 0;
                flagV = quotient > 0xFFFF;
                if (flagV) {
                    flagN = true;
                } else {
                    d[reg2] = quotient | (remainder << 16);
                    flagN = quotient & 0x8000;
                }
                m_cycles += 140;
            }
            m_cycles += operandCycles(1);
            return;
        }
        if ((op & 0x01F0) == 0x0100) { // SBCD / ABCD
            const bool memory = op & 8;
            Operand source{Kind::Data, reg}, target{Kind::Data, reg2};
            if (memory) {
                source = Locate(4, reg, 0);
                target = Locate(4, reg2, 0);
            }
            const int x = Load(source, 0), y = Load(target, 0);
            int result;
            if (isAnd) {
                int low = (x & 15) + (y & 15) + flagX;
                result = x + y + flagX;
                if (low > 9) {
                    result += 6;
                }
                flagC = flagX = result > 0x99;
                if (flagC) {
                    result += 0x60;
                }
            } else {
                int low = (y & 15) - (x & 15) - flagX;
                result = y - x - flagX;
                if (low < 0) {
                    result -= 6;
                }
                flagC = flagX = result < 0;
                if (flagC) {
                    result -= 0x60;
                }
            }
            flagZ = flagZ && (result & 0xFF) == 0;
            flagN = result & 0x80;
            Store(target, 0, static_cast<uint32_t>(result) & 0xFF);
            m_cycles += memory ? 18 : 6;
            return;
        }
        if (isAnd && (op & 0x0100) && (mode < 2) && size != 0) { // EXG
            if (size == 1) {
                uint32_t &x = mode == 0 ? d[reg2] : a[reg2], &y = mode == 0 ? d[reg] : a[reg];
                std::swap(x, y);
            } else {
                std::swap(d[reg2], a[reg]);
            }
            m_cycles += 6;
            return;
        }
        // OR, AND: into the data register, or from it into memory
        const Operand operand = Locate(mode, reg, size);
        const uint32_t value = Load(operand, size);
        const uint32_t result = isAnd ? (value & d[reg2]) : (value | d[reg2]);
        Logic(result, size);
        if (op & 0x0100) {
            Store(operand, size, result);
            m_cycles += operandCycles(size) + (size == 2 ? 12 : 8);
        } else {
            d[reg2] = (d[reg2] & ~kMask[size]) | (result & kMask[size]);
            m_cycles += operandCycles(size) + (size == 2 ? 6 : 4);
        }
        return;
    }
    case 0x9:
    case 0xD: {
        const bool isAdd = (op >> 12) == 0xD;
        if (size == 3) { // SUBA, ADDA
            const unsigned sz = (op & 0x0100) ? 2 : 1;
            const uint32_t value = static_cast<uint32_t>(Extend(Load(Locate(mode, reg, sz), sz), sz));
            a[reg2] = isAdd ? a[reg2] + value : a[reg2] - value;
            m_cycles += operandCycles(sz) + (sz == 2 ? 6 : 8);
            return;
        }
        if ((op & 0x0130) == 0x0100) { // SUBX, ADDX
            const bool memory = op & 8;
            Operand source{Kind::Data, reg}, target{Kind::Data, reg2};
            if (memory) {
                source = Locate(4, reg, size);
                target = Locate(4, reg2, size);
            }
            const uint32_t x = Load(source, size), y = Load(target, size);
            Store(target, size, isAdd ? Add(y, x, size, true, true) : Sub(y, x, size, true, true));
            m_cycles += memory ? (size == 2 ? 30 : 18) : (size == 2 ? 8 : isAdd ? 4 : 6);
            return;
        }
        const Operand operand = Locate(mode, reg, size);
        const uint32_t value = Load(operand, size);
        if (op & 0x0100) {
            Store(operand, size, isAdd ? Add(value, d[reg2], size, false, false) : Sub(value, d[reg2], size, false, false));
            m_cycles += operandCycles(size) + (size == 2 ? 12 : 8);
        } else {
            const uint32_t result = isAdd ? Add(d[reg2], value, size, false, false) : Sub(d[reg2], value, size, false, false);
            d[reg2] = (d[reg2] & ~kMask[size]) | result;
            m_cycles += operandCycles(size) + (size == 2 ? 6 : 4);
        }
        return;
    }
    case 0xB: {
        if (size == 3) { // CMPA
            const unsigned sz = (op & 0x0100) ? 2 : 1;
            const uint32_t value = static_cast<uint32_t>(Extend(Load(Locate(mode, reg, sz), sz), sz));
            Sub(a[reg2], value, 2, false, false, false);
            m_cycles += 6 + operandCycles(sz);
            return;
        }
        if (!(op & 0x0100)) { // CMP
            const uint32_t value = Load(Locate(mode, reg, size), size);
            Sub(d[reg2], value, size, false, false, false);
            m_cycles += operandCycles(size) + (size == 2 ? 6 : 4);
            return;
        }
        if (mode == 1) { // CMPM
            const uint32_t x = Load(Locate(3, reg, size), size);
            const uint32_t y = Load(Locate(3, reg2, size), size);
            Sub(y, x, size, false, false, false);
            m_cycles += size == 2 ? 20 : 12;
            return;
        }
        // EOR
        const Operand operand = Locate(mode, reg, size);
        const uint32_t result = Load(operand, size) ^ (d[reg2] & kMask[size]);
        Logic(result, size);
        Store(operand, size, result);
        m_cycles += operandCycles(size) + (size == 2 ? 12 : 8);
        return;
    }
    case 0xE: { // shifts and rotates
        const bool left = op & 0x0100;
        if (size == 3) { // of a word of memory, by one
            const Operand target = Locate(mode, reg, 1);
            Store(target, 1, Shift(reg2 & 3, left, Load(target, 1), 1, 1));
            m_cycles += 8 + operandCycles(1);
            return;
        }
        const unsigned count = (op & 0x0020) ? (d[reg2] & 63) : (reg2 ? reg2 : 8);
        const uint32_t result = Shift((op >> 3) & 3, left, d[reg], count, size);
        d[reg] = (d[reg] & ~kMask[size]) | result;
        m_cycles += (size == 2 ? 8 : 6) + 2 * count;
        return;
    }
    default: break;
    }
    Illegal(op, at);
}

} // namespace satrecomp::sound
