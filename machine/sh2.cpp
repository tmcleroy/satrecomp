// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "sh2.h"

#include "machine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace satrecomp::machine {

uint64 g_activity = 0;
uint64 g_hardware = 0;
std::vector<HardwareWrite> *g_hardwareWrites = nullptr;
const uint8 *g_codeBase = nullptr;
const uint8 *g_codeMap = nullptr;
size_t g_codeSize = 0;
uint64 g_codeEpoch = 1;

Op g_opKinds[0x10000];
uint8 g_opCycles[0x10000];

namespace {
    // The time an instruction is counted as: a fixed number of cycles by its kind
    unsigned NominalCycles(Op op) {
        switch (op) {
        case Op::BRA: case Op::BRAF: case Op::BSR: case Op::BSRF: case Op::JMP: case Op::JSR: case Op::RTS: case Op::BT:
        case Op::BF: case Op::BTS: case Op::BFS: case Op::MUL: case Op::DMULS: case Op::DMULU: return 2;
        case Op::MACW: case Op::MACL: case Op::AND_M: case Op::OR_M: case Op::XOR_M: case Op::TST_M: case Op::LDC_GBR_M:
        case Op::LDC_SR_M: case Op::LDC_VBR_M: case Op::STC_GBR_M: case Op::STC_SR_M: case Op::STC_VBR_M: return 3;
        case Op::TAS: case Op::RTE: return 4;
        default: return 1;
        }
    }
    const bool g_tablesBuilt = [] {
        for (uint32 opcode = 0; opcode < 0x10000; opcode++) {
            Op kind = Op::Illegal;
            for (const OpPattern &pattern : kOpPatterns) {
                if ((opcode & pattern.mask) == pattern.match) {
                    kind = pattern.op;
                    break;
                }
            }
            g_opKinds[opcode] = kind;
            g_opCycles[opcode] = static_cast<uint8>(NominalCycles(kind));
        }
        return true;
    }();

    uint16 Get16(const uint8 *p) {
        return static_cast<uint16>((p[0] << 8) | p[1]);
    }
    uint32 Get32(const uint8 *p) {
        return (static_cast<uint32>(p[0]) << 24) | (static_cast<uint32>(p[1]) << 16) | (static_cast<uint32>(p[2]) << 8) | p[3];
    }
    void Put16(uint8 *p, uint16 v) {
        p[0] = static_cast<uint8>(v >> 8);
        p[1] = static_cast<uint8>(v);
    }
    void Put32(uint8 *p, uint32 v) {
        p[0] = static_cast<uint8>(v >> 24);
        p[1] = static_cast<uint8>(v >> 16);
        p[2] = static_cast<uint8>(v >> 8);
        p[3] = static_cast<uint8>(v);
    }
    // Interrupt sources, in the order they win between equal levels
    enum : uint8 { kSourceNone, kSourceNMI, kSourceExternal, kSourceDivider, kSourceDma0, kSourceDma1, kSourceCapture, kSourceCompare, kSourceOverflow };
} // namespace

bool IllegalInSlot(Op op) {
    switch (op) {
    case Op::BRA: case Op::BRAF: case Op::BSR: case Op::BSRF: case Op::JMP: case Op::JSR: case Op::RTS: case Op::RTE:
    case Op::BT: case Op::BF: case Op::BTS: case Op::BFS: case Op::TRAPA: case Op::Illegal: return true;
    default: return false;
    }
}

SH2::SH2(Machine &machine, bool master)
    : m_machine(machine)
    , m_master(master) {
    (void)g_tablesBuilt;
}

void SH2::Reset(bool hard) {
    (void)hard;
    R.fill(0);
    PR = GBR = VBR = 0;
    MAC.u64 = 0;
    SR.u32 = 0;
    SR.ILevel = 15;
    m_intrFlags = {};
    m_delaySlot = false;
    m_delaySlotTarget = 0;
    m_cyclesExecuted = 0;
    m_recompResume = 0;
    m_recompYield = m_recompWaiting = m_sleep = m_yieldRequest = false;
    INTC = {};
    m_frt = {};
    m_divu = {};
    m_dma[0] = m_dma[1] = {};
    m_dmaOperation = 0;
    m_cacheControl = 0;
    // (the reset vector: PC and the stack pointer from the first two entries of the table in ROM)
    PC = Read32(0x00000000);
    R[15] = Read32(0x00000004);
}

// ---------------------------------------------------------------------------------------------------------
// Memory

uint8 SH2::Read8(uint32 a) {
    if ((a & 0xC6000000u) == 0x06000000u) [[likely]] {
        return m_machine.ramHigh[a & 0xFFFFFu];
    }
    ++g_activity;
    g_hardware += (a & 0xC7F00000u) != 0x00200000u;
    switch (a >> 29) {
    case 0: case 1: case 2: return m_machine.BusRead8(a & 0x07FFFFFFu);
    case 6: return m_cacheData[a & 0xFFF];
    case 7: return a >= 0xFFFFFE00u ? static_cast<uint8>(ReadRegister(a & 0x1FF, 1)) : 0;
    default: return 0;
    }
}
uint16 SH2::Read16(uint32 a) {
    a &= ~1u;
    if ((a & 0xC6000000u) == 0x06000000u) [[likely]] {
        return Get16(&m_machine.ramHigh[a & 0xFFFFFu]);
    }
    ++g_activity;
    g_hardware += (a & 0xC7F00000u) != 0x00200000u;
    switch (a >> 29) {
    case 0: case 1: case 2: return m_machine.BusRead16(a & 0x07FFFFFFu);
    case 6: return Get16(&m_cacheData[a & 0xFFE]);
    case 7: return a >= 0xFFFFFE00u ? static_cast<uint16>(ReadRegister(a & 0x1FF, 2)) : 0;
    default: return 0;
    }
}
uint32 SH2::Read32(uint32 a) {
    a &= ~3u;
    if ((a & 0xC6000000u) == 0x06000000u) [[likely]] {
        return Get32(&m_machine.ramHigh[a & 0xFFFFFu]);
    }
    ++g_activity;
    g_hardware += (a & 0xC7F00000u) != 0x00200000u;
    switch (a >> 29) {
    case 0: case 1: case 2: return m_machine.BusRead32(a & 0x07FFFFFFu);
    case 6: return Get32(&m_cacheData[a & 0xFFC]);
    case 7: return a >= 0xFFFFFE00u ? ReadRegister(a & 0x1FF, 4) : 0;
    default: return 0;
    }
}
// Debugging aid, SATRECOMP_WRITERS=lo-hi (hex; run with SATRECOMP_NO_BLOCKS=1): which instructions write
// in that range of addresses, counted and listed at exit
namespace {
    struct Writers {
        uint32 lo = 1, hi = 0;
        std::map<uint64, uint64> counts; // (cpu, PC)
        Writers() {
            if (const char *v = getenv("SATRECOMP_WRITERS")) {
                sscanf(v, "%x-%x", &lo, &hi);
            }
        }
        ~Writers() {
            for (const auto &[key, count] : counts) {
                fprintf(stderr, "writer %c %08X: %llu\n", (key >> 32) ? 'M' : 'S', static_cast<uint32>(key), static_cast<unsigned long long>(count));
            }
        }
    } g_writers;
} // namespace
// Debugging aid, SATRECOMP_CALL_GRAPH=path (run with SATRECOMP_NO_BLOCKS=1): every call made (jsr, bsr,
// bsrf), counted, written at exit as "cpu site target count" (tools/callgraph.py reads it)
namespace {
    struct CallGraph {
        const char *path = getenv("SATRECOMP_CALL_GRAPH");
        std::map<std::pair<uint64, uint32>, uint64> counts; // ((cpu, site), target)
        ~CallGraph() {
            if (FILE *f = path ? fopen(path, "w") : nullptr) {
                for (const auto &[key, count] : counts) {
                    fprintf(f, "%c %08X %08X %llu\n", (key.first >> 32) ? 'M' : 'S', static_cast<uint32>(key.first), key.second,
                            static_cast<unsigned long long>(count));
                }
                fclose(f);
            }
        }
    } g_callGraph;
} // namespace
inline void SH2::NoteCall(uint32 site, uint32 target) {
    if (g_callGraph.path) [[unlikely]] {
        g_callGraph.counts[{(static_cast<uint64>(m_master) << 32) | site, target}]++;
    }
}

inline void SH2::NoteWriter(uint32 a) {
    const uint32 address = a & 0x07FFFFFFu;
    if (address >= g_writers.lo && address < g_writers.hi) [[unlikely]] {
        g_writers.counts[(static_cast<uint64>(m_master) << 32) | PC]++;
    }
}

void SH2::Write8(uint32 a, uint8 v) {
    ++g_activity;
    NoteWriter(a);
    if ((a & 0xC6000000u) == 0x06000000u) [[likely]] {
        uint8 *p = &m_machine.ramHigh[a & 0xFFFFFu];
        *p = v;
        NoteWrite(p);
        return;
    }
    if ((a & 0xC7F00000u) != 0x00200000u) {
        ++g_hardware;
        if (g_hardwareWrites) [[unlikely]] {
            g_hardwareWrites->push_back({a, v, 1});
        }
    }
    switch (a >> 29) {
    case 0: case 1: m_machine.BusWrite8(a & 0x07FFFFFFu, v); break;
    case 2: break; // (the cache's associative purge area: a write there drops a line from the cache; there is no cache here)
    case 6: m_cacheData[a & 0xFFF] = v; break;
    case 7:
        if (a >= 0xFFFFFE00u) {
            WriteRegister(a & 0x1FF, 1, v);
        }
        break;
    default: break;
    }
}
void SH2::Write16(uint32 a, uint16 v) {
    a &= ~1u;
    ++g_activity;
    NoteWriter(a);
    if ((a & 0xC6000000u) == 0x06000000u) [[likely]] {
        uint8 *p = &m_machine.ramHigh[a & 0xFFFFFu];
        Put16(p, v);
        NoteWrite(p);
        return;
    }
    if ((a & 0xC7F00000u) != 0x00200000u) {
        ++g_hardware;
        if (g_hardwareWrites) [[unlikely]] {
            g_hardwareWrites->push_back({a, v, 2});
        }
    }
    switch (a >> 29) {
    case 0: case 1: m_machine.BusWrite16(a & 0x07FFFFFFu, v); break;
    case 2: break; // (the cache's associative purge area: a write there drops a line from the cache; there is no cache here)
    case 6: Put16(&m_cacheData[a & 0xFFE], v); break;
    case 7:
        if (a >= 0xFFFFFE00u) {
            WriteRegister(a & 0x1FF, 2, v);
        }
        break;
    default: break;
    }
}
void SH2::Write32(uint32 a, uint32 v) {
    a &= ~3u;
    ++g_activity;
    NoteWriter(a);
    // (debugging aid, SATRECOMP_WATCH_WRITE=address: long word writes there by interpreted code)
    static const uint32 watch = getenv("SATRECOMP_WATCH_WRITE") ? static_cast<uint32>(strtoul(getenv("SATRECOMP_WATCH_WRITE"), nullptr, 16)) : 1;
    if (((a ^ watch) & 0x07FFFFFFu) == 0) [[unlikely]] {
        fprintf(stderr, "write %08X = %08X at PC %08X PR %08X\n", a, v, PC, PR);
    }
    if ((a & 0xC6000000u) == 0x06000000u) [[likely]] {
        uint8 *p = &m_machine.ramHigh[a & 0xFFFFFu];
        Put32(p, v);
        NoteWrite(p);
        return;
    }
    if ((a & 0xC7F00000u) != 0x00200000u) {
        ++g_hardware;
        if (g_hardwareWrites) [[unlikely]] {
            g_hardwareWrites->push_back({a, v, 4});
        }
    }
    switch (a >> 29) {
    case 0: case 1: m_machine.BusWrite32(a & 0x07FFFFFFu, v); break;
    case 2: break; // (the cache's associative purge area: a write there drops a line from the cache; there is no cache here)
    case 6: Put32(&m_cacheData[a & 0xFFC], v); break;
    case 7:
        if (a >= 0xFFFFFE00u) {
            WriteRegister(a & 0x1FF, 4, v);
        }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------
// On-chip peripherals (addresses 0xFFFFFE00-0xFFFFFFFF; `address` is the low 9 bits)

// The free-running timer counts up every 8, 32 or 128 cycles; compare matches and overflow set flags
void SH2::SyncTimer() {
    const uint64 now = m_cyclesExecuted;
    if (now <= m_timerSynced) {
        return;
    }
    static constexpr unsigned kShift[4] = {3, 5, 7, 3};
    const unsigned shift = kShift[m_frt.TCR & 3];
    m_frt.partial += now - m_timerSynced;
    m_timerSynced = now;
    const uint64 ticks = m_frt.partial >> shift;
    m_frt.partial &= (1ull << shift) - 1;
    if (!ticks) {
        return;
    }
    const uint64 before = m_frt.FRC, after = before + ticks;
    uint8 flags = 0;
    if (before < m_frt.OCRA && after >= m_frt.OCRA) {
        flags |= 0x08;
    }
    if (before < m_frt.OCRB && after >= m_frt.OCRB) {
        flags |= 0x04;
    }
    if (after > 0xFFFF) {
        flags |= 0x02;
    }
    m_frt.FRC = static_cast<uint16>(after);
    if ((flags & 0x08) && (m_frt.FTCSR & 0x01)) {
        m_frt.FRC = static_cast<uint16>(after - m_frt.OCRA); // (cleared at the compare match)
    }
    if (flags & ~m_frt.FTCSR) {
        m_frt.FTCSR |= flags;
        if (flags & m_frt.TIER) {
            RecalcInterrupts();
        }
    }
}

void SH2::TriggerInputCapture() {
    SyncTimer();
    m_frt.FICR = m_frt.FRC;
    m_frt.FTCSR |= 0x80;
    RecalcInterrupts();
}

uint32 SH2::ReadRegister(uint32 address, unsigned size) {
    if (address < 0x100) {
        if (size > 1) { // (the registers down here are bytes, or words read a byte at a time)
            uint32 value = 0;
            for (unsigned i = 0; i < size; i++) {
                value = (value << 8) | ReadRegister(address + i, 1);
            }
            return value;
        }
        switch (address) {
        case 0x10: return m_frt.TIER;
        case 0x11: SyncTimer(); return m_frt.FTCSR;
        case 0x12: SyncTimer(); m_frt.latch = static_cast<uint8>(m_frt.FRC); return m_frt.FRC >> 8;
        case 0x13: return m_frt.latch;
        case 0x14: return ((m_frt.TOCR & 0x10) ? m_frt.OCRB : m_frt.OCRA) >> 8;
        case 0x15: return ((m_frt.TOCR & 0x10) ? m_frt.OCRB : m_frt.OCRA) & 0xFF;
        case 0x16: return m_frt.TCR;
        case 0x17: return m_frt.TOCR;
        case 0x18: m_frt.latch = static_cast<uint8>(m_frt.FICR); return m_frt.FICR >> 8;
        case 0x19: return m_frt.latch;
        case 0x60: return INTC.IPRB >> 8;
        case 0x61: return INTC.IPRB & 0xFF;
        case 0x62: return INTC.VCRA >> 8;
        case 0x63: return INTC.VCRA & 0xFF;
        case 0x64: return INTC.VCRB >> 8;
        case 0x65: return INTC.VCRB & 0xFF;
        case 0x66: return INTC.VCRC >> 8;
        case 0x67: return INTC.VCRC & 0xFF;
        case 0x68: return INTC.VCRD >> 8;
        case 0x69: return INTC.VCRD & 0xFF;
        case 0x71: return m_dma[0].request;
        case 0x72: return m_dma[1].request;
        case 0x92: return m_cacheControl;
        case 0xE0: return INTC.ICR >> 8;
        case 0xE1: return INTC.ICR & 0xFF;
        case 0xE2: return INTC.IPRA >> 8;
        case 0xE3: return INTC.IPRA & 0xFF;
        case 0xE4: return INTC.VCRWDT >> 8;
        case 0xE5: return INTC.VCRWDT & 0xFF;
        default: return 0;
        }
    }
    uint32 value = 0;
    switch (address & 0x1FC) {
    case 0x100: case 0x120: value = m_divu.DVSR; break;
    case 0x104: case 0x124: value = m_divu.DVDNT; break;
    case 0x108: case 0x128: value = m_divu.DVCR; break;
    case 0x10C: case 0x12C: value = INTC.VCRDIV; break;
    case 0x110: case 0x130: value = m_divu.DVDNTH; break;
    case 0x114: case 0x134: value = m_divu.DVDNTL; break;
    case 0x118: case 0x138: value = m_divu.DVDNTUH; break;
    case 0x11C: case 0x13C: value = m_divu.DVDNTUL; break;
    case 0x180: case 0x190: value = m_dma[(address >> 4) & 1].source; break;
    case 0x184: case 0x194: value = m_dma[(address >> 4) & 1].target; break;
    case 0x188: case 0x198: value = m_dma[(address >> 4) & 1].count; break;
    case 0x18C: case 0x19C: value = m_dma[(address >> 4) & 1].control; break;
    case 0x1A0: value = INTC.VCRDMA[0]; break;
    case 0x1A8: value = INTC.VCRDMA[1]; break;
    case 0x1B0: value = m_dmaOperation; break;
    case 0x1E0: value = m_master ? 0x03F0 : 0x83F0; break; // BCR1: which of the two CPUs this is
    default: break;
    }
    // (a word or byte of a long word register)
    if (size == 2) {
        return (address & 2) ? (value & 0xFFFF) : (value >> 16);
    }
    if (size == 1) {
        return (value >> ((3 - (address & 3)) * 8)) & 0xFF;
    }
    return value;
}

void SH2::WriteRegister(uint32 address, unsigned size, uint32 value) {
    if (address < 0x100) {
        if (size > 1) {
            for (unsigned i = 0; i < size; i++) {
                WriteRegister(address + i, 1, (value >> ((size - 1 - i) * 8)) & 0xFF);
            }
            return;
        }
        const uint8 byte = static_cast<uint8>(value);
        auto high = [&](uint16 &reg) { reg = static_cast<uint16>((reg & 0x00FF) | (byte << 8)); };
        auto low = [&](uint16 &reg) { reg = static_cast<uint16>((reg & 0xFF00) | byte); };
        switch (address) {
        case 0x10: m_frt.TIER = (byte & 0x8E) | 0x01; RecalcInterrupts(); break;
        case 0x11: // (flags can only be cleared)
            SyncTimer();
            m_frt.FTCSR = static_cast<uint8>((m_frt.FTCSR & byte & 0x8E) | (byte & 0x01));
            RecalcInterrupts();
            break;
        case 0x12: m_frt.latch = byte; break;
        case 0x13: SyncTimer(); m_frt.FRC = static_cast<uint16>((m_frt.latch << 8) | byte); break;
        case 0x14: m_frt.latch = byte; break;
        case 0x15: ((m_frt.TOCR & 0x10) ? m_frt.OCRB : m_frt.OCRA) = static_cast<uint16>((m_frt.latch << 8) | byte); break;
        case 0x16: SyncTimer(); m_frt.TCR = byte & 0x83; break;
        case 0x17: m_frt.TOCR = (byte & 0x13) | 0xE0; break;
        case 0x60: high(INTC.IPRB); RecalcInterrupts(); break;
        case 0x61: break;
        case 0x62: high(INTC.VCRA); break;
        case 0x63: low(INTC.VCRA); break;
        case 0x64: high(INTC.VCRB); break;
        case 0x65: low(INTC.VCRB); break;
        case 0x66: high(INTC.VCRC); break;
        case 0x67: low(INTC.VCRC); break;
        case 0x68: high(INTC.VCRD); break;
        case 0x69: break;
        case 0x71: m_dma[0].request = byte & 3; break;
        case 0x72: m_dma[1].request = byte & 3; break;
        case 0x92: m_cacheControl = byte & 0xCF; break;
        case 0xE0: high(INTC.ICR); break;
        case 0xE1: low(INTC.ICR); break;
        case 0xE2: high(INTC.IPRA); RecalcInterrupts(); break;
        case 0xE3: low(INTC.IPRA); RecalcInterrupts(); break;
        case 0xE4: high(INTC.VCRWDT); break;
        case 0xE5: low(INTC.VCRWDT); break;
        default: break;
        }
        return;
    }
    switch (address & 0x1FC) {
    case 0x100: case 0x120: m_divu.DVSR = value; break;
    case 0x104: case 0x124:
        m_divu.DVDNT = value;
        Divide32();
        break;
    case 0x108: case 0x128:
        m_divu.DVCR = value & 3;
        RecalcInterrupts();
        break;
    case 0x10C: case 0x12C: INTC.VCRDIV = static_cast<uint16>(value); break;
    case 0x110: case 0x130: m_divu.DVDNTH = value; break;
    case 0x114: case 0x134:
        m_divu.DVDNTL = value;
        Divide64();
        break;
    case 0x118: case 0x138: m_divu.DVDNTUH = value; break;
    case 0x11C: case 0x13C: m_divu.DVDNTUL = value; break;
    case 0x180: case 0x190: m_dma[(address >> 4) & 1].source = value; break;
    case 0x184: case 0x194: m_dma[(address >> 4) & 1].target = value; break;
    case 0x188: case 0x198: m_dma[(address >> 4) & 1].count = value & 0xFFFFFF; break;
    case 0x18C: case 0x19C: {
        DmaChannel &channel = m_dma[(address >> 4) & 1];
        // (the "transfer ended" flag can only be cleared)
        channel.control = (value & 0xFFFD) | (channel.control & value & 2);
        RunDma((address >> 4) & 1);
        RecalcInterrupts();
        break;
    }
    case 0x1A0: INTC.VCRDMA[0] = value & 0xFF; break;
    case 0x1A8: INTC.VCRDMA[1] = value & 0xFF; break;
    case 0x1B0:
        m_dmaOperation = (value & 0x9) | (m_dmaOperation & value & 0x6);
        RunDma(0);
        RunDma(1);
        RecalcInterrupts();
        break;
    default: break;
    }
}

// The divider: 32 / 32 bits and 64 / 32 bits, signed, with the results the hardware gives on overflow
void SH2::Divide32() {
    const sint32 dividend = static_cast<sint32>(m_divu.DVDNT);
    const sint32 divisor = static_cast<sint32>(m_divu.DVSR);
    if (divisor != 0) {
        if (dividend == INT32_MIN && divisor == -1) {
            m_divu.DVDNTL = m_divu.DVDNT = 0x80000000u;
            m_divu.DVDNTH = 0;
        } else {
            m_divu.DVDNTL = m_divu.DVDNT = static_cast<uint32>(dividend / divisor);
            m_divu.DVDNTH = static_cast<uint32>(dividend % divisor);
        }
    } else {
        m_divu.DVDNTH = static_cast<uint32>(dividend >> 29);
        if (m_divu.DVCR & 2) {
            m_divu.DVDNTL = m_divu.DVDNT = (static_cast<uint32>(dividend) << 3) | ((~static_cast<uint32>(dividend) >> 31) & 7);
        } else {
            m_divu.DVDNTL = m_divu.DVDNT = dividend < 0 ? 0x80000000u : 0x7FFFFFFFu;
        }
        m_divu.DVCR |= 1;
        RecalcInterrupts();
    }
    m_divu.DVDNTUH = m_divu.DVDNTH;
    m_divu.DVDNTUL = m_divu.DVDNTL;
}

void SH2::Divide64() {
    sint64 dividend = static_cast<sint64>((static_cast<uint64>(m_divu.DVDNTH) << 32) | m_divu.DVDNTL);
    const sint32 divisor = static_cast<sint32>(m_divu.DVSR);
    bool overflow = false;
    if (divisor == 0) {
        overflow = true;
    } else if (dividend == -0x80000000ll && (divisor == 1 || divisor == -1)) {
        m_divu.DVDNTH = m_divu.DVDNTUH = 0;
        m_divu.DVDNT = m_divu.DVDNTL = m_divu.DVDNTUL = 0x80000000u;
        return;
    } else if (dividend == INT64_MIN && divisor == -1) {
        overflow = true;
    } else {
        const sint64 quotient = dividend / divisor;
        const sint32 remainder = static_cast<sint32>(dividend % divisor);
        if (((quotient == INT32_MIN && divisor > 0) || (quotient == 0x80000000ll && divisor < 0)) && remainder == 0) {
            m_divu.DVDNTL = m_divu.DVDNT = static_cast<uint32>(quotient);
            m_divu.DVDNTH = static_cast<uint32>(remainder);
        } else if (quotient <= INT32_MIN || quotient > INT32_MAX) {
            overflow = true;
        } else {
            m_divu.DVDNTL = m_divu.DVDNT = static_cast<uint32>(quotient);
            m_divu.DVDNTH = static_cast<uint32>(remainder);
        }
    }
    if (overflow) {
        // (the hardware notices after three steps of its division, and leaves what those made)
        const sint64 original = dividend;
        bool q = dividend < 0;
        const bool m = divisor < 0;
        for (int i = 0; i < 3; i++) {
            if (q == m) {
                dividend = static_cast<sint64>(static_cast<uint64>(dividend) - (static_cast<uint64>(static_cast<sint64>(divisor)) << 32));
            } else {
                dividend = static_cast<sint64>(static_cast<uint64>(dividend) + (static_cast<uint64>(static_cast<sint64>(divisor)) << 32));
            }
            q = dividend < 0;
            dividend = static_cast<sint64>((static_cast<uint64>(dividend) << 1) | (q == m));
        }
        if (m_divu.DVCR & 2) {
            m_divu.DVDNTL = m_divu.DVDNT = static_cast<uint32>(dividend);
        } else {
            m_divu.DVDNTL = m_divu.DVDNT = static_cast<sint32>((original >> 32) ^ divisor) < 0 ? 0x80000000u : 0x7FFFFFFFu;
        }
        m_divu.DVDNTH = static_cast<uint32>(dividend >> 32);
        m_divu.DVCR |= 1;
        RecalcInterrupts();
    }
    m_divu.DVDNTUH = m_divu.DVDNTH;
    m_divu.DVDNTUL = m_divu.DVDNTL;
}

// The DMA controller: a channel that is enabled copies its block at once
void SH2::RunDma(unsigned index) {
    DmaChannel &channel = m_dma[index];
    if (!(channel.control & 1) || (channel.control & 2) || (m_dmaOperation & 7) != 1) {
        return;
    }
    static constexpr uint32 kUnit[4] = {1, 2, 4, 4};
    const uint32 unit = kUnit[(channel.control >> 10) & 3];
    const uint32 sourceMode = (channel.control >> 12) & 3, targetMode = (channel.control >> 14) & 3;
    auto step = [unit](uint32 &address, uint32 mode) {
        if (mode == 1) {
            address += unit;
        } else if (mode == 2) {
            address -= unit;
        }
    };
    uint32 count = channel.count ? channel.count : 0x1000000;
    while (count--) {
        switch (unit) {
        case 1: Write8(channel.target, Read8(channel.source)); break;
        case 2: Write16(channel.target, Read16(channel.source)); break;
        default: Write32(channel.target, Read32(channel.source)); break;
        }
        step(channel.source, sourceMode);
        step(channel.target, targetMode);
    }
    channel.count = 0;
    channel.control |= 2;
}

// ---------------------------------------------------------------------------------------------------------
// Interrupts

void SH2::SetExternalInterrupt(uint8 level, uint8 vector) {
    INTC.externalLevel = level;
    INTC.externalVector = vector;
    RecalcInterrupts();
}

void SH2::SetNMI() {
    INTC.nmi = true;
    RecalcInterrupts();
}

void SH2::RecalcInterrupts() {
    uint8 level = 0, vector = 0, source = kSourceNone;
    auto consider = [&](bool active, uint32 itsLevel, uint32 itsVector, uint8 itsSource) {
        if (active && itsLevel > level) {
            level = static_cast<uint8>(itsLevel);
            vector = static_cast<uint8>(itsVector);
            source = itsSource;
        }
    };
    consider(INTC.nmi, 16, 11, kSourceNMI);
    consider(INTC.externalLevel != 0, INTC.externalLevel, INTC.externalVector, kSourceExternal);
    consider((m_divu.DVCR & 3) == 3, INTC.IPRA >> 12, INTC.VCRDIV & 0x7F, kSourceDivider);
    consider((m_dma[0].control & 6) == 6, (INTC.IPRA >> 8) & 15, INTC.VCRDMA[0] & 0x7F, kSourceDma0);
    consider((m_dma[1].control & 6) == 6, (INTC.IPRA >> 8) & 15, INTC.VCRDMA[1] & 0x7F, kSourceDma1);
    const uint32 timerLevel = (INTC.IPRB >> 8) & 15;
    const uint8 timerFlags = m_frt.FTCSR & m_frt.TIER;
    consider(timerFlags & 0x80, timerLevel, (INTC.VCRC >> 8) & 0x7F, kSourceCapture);
    consider(timerFlags & 0x0C, timerLevel, INTC.VCRC & 0x7F, kSourceCompare);
    consider(timerFlags & 0x02, timerLevel, (INTC.VCRD >> 8) & 0x7F, kSourceOverflow);
    INTC.pending.level = level;
    INTC.pending.vector = vector;
    INTC.pending.source = source;
    const bool pending = level > SR.ILevel;
    if (pending && !m_intrFlags.pending) {
        ++g_activity;
    }
    m_intrFlags.pending = pending;
}

void SH2::EnterException(uint8 vector, uint32 returnAddress) {
    R[15] -= 4;
    Write32(R[15], SR.u32);
    R[15] -= 4;
    Write32(R[15], returnAddress);
    PC = Read32(VBR + static_cast<uint32>(vector) * 4);
}

void SH2::TakeInterrupt() {
    const uint8 level = INTC.pending.level, vector = INTC.pending.vector, source = INTC.pending.source;
    EnterException(vector, PC);
    SR.ILevel = level > 15 ? 15 : level;
    if (source == kSourceNMI) {
        INTC.nmi = false;
    } else if (source == kSourceExternal) {
        m_machine.AcknowledgeExternalInterrupt(*this); // (the line's source takes it back, or raises the next)
    }
    RecalcInterrupts();
}

// ---------------------------------------------------------------------------------------------------------
// Running

uint64 SH2::Advance(uint64 cycles, uint64 spillover) {
    m_cyclesExecuted = spillover;
    m_timerSynced = spillover;
    while (m_cyclesExecuted < cycles) {
        if (m_yieldRequest) [[unlikely]] {
            m_yieldRequest = false;
            RecalcInterrupts(); // (the flag that made the code stop is the interrupts' again)
            break;
        }
        if (m_sleep) {
            // (asleep until an interrupt)
            if (!m_intrFlags.pending) {
                m_cyclesExecuted = cycles;
                break;
            }
            m_sleep = false;
            PC += 2;
        }
        if (m_dispatch && !m_delaySlot && m_dispatch(*this, cycles, m_dispatchContext)) {
            if (m_recompYield) [[unlikely]] {
                m_recompYield = false;
                break; // (the other CPU's turn, before this one's time is up)
            }
            continue;
        }
        m_cyclesExecuted += RecompInterpretNext();
    }
    SyncTimer();
    return m_cyclesExecuted;
}

uint64 SH2::RecompInterpretNext() {
    if (m_intrFlags.pending && m_intrFlags.allow && !m_delaySlot) {
        TakeInterrupt();
        return 9;
    }
    const uint16 opcode = (PC & 0xC6000000u) == 0x06000000u ? Get16(&m_machine.ramHigh[PC & 0xFFFFEu]) : m_machine.BusRead16(PC & 0x07FFFFFEu);
    m_intrFlags.allow = true;
    Execute(opcode);
    return g_opCycles[opcode];
}

void SH2::Execute(uint16 opcode) {
    const uint32 pc = PC;
    const bool inSlot = m_delaySlot;
    const uint32 slotTarget = m_delaySlotTarget;
    m_delaySlot = false;
    // What PC-relative addressing sees as "PC + 4" (after a branch, that is already where it leads)
    const uint32 pcBase = inSlot ? slotTarget + 2 : pc + 4;
    uint32 next = pc + 2;
    bool jumped = false;

    const uint32 n = (opcode >> 8) & 15, m = (opcode >> 4) & 15;
    const uint32 d4 = opcode & 15, d8 = opcode & 0xFF;
    const sint32 imm = static_cast<sint8>(opcode);
    uint32 &Rn = R[n], &Rm = R[m], &R0 = R[0];
    auto sext8 = [](uint32 v) { return static_cast<uint32>(static_cast<sint32>(static_cast<sint8>(v))); };
    auto sext16 = [](uint32 v) { return static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(v))); };
    auto delayed = [&](uint32 target) { SetupDelaySlot(target); };
    auto writeSR = [&](uint32 value) {
        SR.u32 = value & 0x3F3u;
        m_intrFlags.pending = INTC.pending.level > SR.ILevel;
    };

    switch (g_opKinds[opcode]) {
    case Op::NOP: break;
    case Op::SLEEP:
        m_sleep = true;
        next = pc; // (stays here until an interrupt wakes it)
        break;
    case Op::MOV_R: Rn = Rm; break;
    case Op::MOVB_L: Rn = sext8(Read8(Rm)); break;
    case Op::MOVW_L: Rn = sext16(Read16(Rm)); break;
    case Op::MOVL_L: Rn = Read32(Rm); break;
    case Op::MOVB_L0: Rn = sext8(Read8(Rm + R0)); break;
    case Op::MOVW_L0: Rn = sext16(Read16(Rm + R0)); break;
    case Op::MOVL_L0: Rn = Read32(Rm + R0); break;
    case Op::MOVB_L4: R0 = sext8(Read8(Rm + d4)); break;
    case Op::MOVW_L4: R0 = sext16(Read16(Rm + d4 * 2)); break;
    case Op::MOVL_L4: Rn = Read32(Rm + d4 * 4); break;
    case Op::MOVB_LG: R0 = sext8(Read8(GBR + d8)); break;
    case Op::MOVW_LG: R0 = sext16(Read16(GBR + d8 * 2)); break;
    case Op::MOVL_LG: R0 = Read32(GBR + d8 * 4); break;
    case Op::MOVB_M: Write8(Rn - 1, static_cast<uint8>(Rm)); Rn -= 1; break;
    case Op::MOVW_M: Write16(Rn - 2, static_cast<uint16>(Rm)); Rn -= 2; break;
    case Op::MOVL_M: Write32(Rn - 4, Rm); Rn -= 4; break;
    case Op::MOVB_P: { const uint32 v = sext8(Read8(Rm)); if (n != m) { Rm += 1; } Rn = v; break; }
    case Op::MOVW_P: { const uint32 v = sext16(Read16(Rm)); if (n != m) { Rm += 2; } Rn = v; break; }
    case Op::MOVL_P: { const uint32 v = Read32(Rm); if (n != m) { Rm += 4; } Rn = v; break; }
    case Op::MOVB_S: Write8(Rn, static_cast<uint8>(Rm)); break;
    case Op::MOVW_S: Write16(Rn, static_cast<uint16>(Rm)); break;
    case Op::MOVL_S: Write32(Rn, Rm); break;
    case Op::MOVB_S0: Write8(Rn + R0, static_cast<uint8>(Rm)); break;
    case Op::MOVW_S0: Write16(Rn + R0, static_cast<uint16>(Rm)); break;
    case Op::MOVL_S0: Write32(Rn + R0, Rm); break;
    case Op::MOVB_S4: Write8(Rm + d4, static_cast<uint8>(R0)); break;
    case Op::MOVW_S4: Write16(Rm + d4 * 2, static_cast<uint16>(R0)); break;
    case Op::MOVL_S4: Write32(Rn + d4 * 4, Rm); break;
    case Op::MOVB_SG: Write8(GBR + d8, static_cast<uint8>(R0)); break;
    case Op::MOVW_SG: Write16(GBR + d8 * 2, static_cast<uint16>(R0)); break;
    case Op::MOVL_SG: Write32(GBR + d8 * 4, R0); break;
    case Op::MOV_I: Rn = static_cast<uint32>(imm); break;
    case Op::MOVW_I: Rn = sext16(Read16(pcBase + d8 * 2)); break;
    case Op::MOVL_I: Rn = Read32((pcBase & ~3u) + d8 * 4); break;
    case Op::MOVA: R0 = (pcBase & ~3u) + d8 * 4; break;
    case Op::MOVT: Rn = SR.T; break;
    case Op::CLRT: SR.T = 0; break;
    case Op::SETT: SR.T = 1; break;
    case Op::EXTUB: Rn = Rm & 0xFF; break;
    case Op::EXTUW: Rn = Rm & 0xFFFF; break;
    case Op::EXTSB: Rn = sext8(Rm); break;
    case Op::EXTSW: Rn = sext16(Rm); break;
    case Op::SWAPB: Rn = (Rm & 0xFFFF0000u) | ((Rm & 0xFF) << 8) | ((Rm >> 8) & 0xFF); break;
    case Op::SWAPW: Rn = (Rm << 16) | (Rm >> 16); break;
    case Op::XTRCT: Rn = (Rn >> 16) | (Rm << 16); break;
    case Op::LDC_GBR_R: GBR = Rn; m_intrFlags.allow = false; break;
    case Op::LDC_SR_R: writeSR(Rn); m_intrFlags.allow = false; break;
    case Op::LDC_VBR_R: VBR = Rn; m_intrFlags.allow = false; break;
    case Op::LDS_MACH_R: MAC.H = Rn; m_intrFlags.allow = false; break;
    case Op::LDS_MACL_R: MAC.L = Rn; m_intrFlags.allow = false; break;
    case Op::LDS_PR_R: PR = Rn; m_intrFlags.allow = false; break;
    case Op::STC_GBR_R: Rn = GBR; m_intrFlags.allow = false; break;
    case Op::STC_SR_R: Rn = SR.u32; m_intrFlags.allow = false; break;
    case Op::STC_VBR_R: Rn = VBR; m_intrFlags.allow = false; break;
    case Op::STS_MACH_R: Rn = MAC.H; m_intrFlags.allow = false; break;
    case Op::STS_MACL_R: Rn = MAC.L; m_intrFlags.allow = false; break;
    case Op::STS_PR_R: Rn = PR; m_intrFlags.allow = false; break;
    case Op::LDC_GBR_M: GBR = Read32(Rn); Rn += 4; m_intrFlags.allow = false; break;
    case Op::LDC_SR_M: { const uint32 v = Read32(Rn); Rn += 4; writeSR(v); m_intrFlags.allow = false; break; }
    case Op::LDC_VBR_M: VBR = Read32(Rn); Rn += 4; m_intrFlags.allow = false; break;
    case Op::LDS_MACH_M: MAC.H = Read32(Rn); Rn += 4; m_intrFlags.allow = false; break;
    case Op::LDS_MACL_M: MAC.L = Read32(Rn); Rn += 4; m_intrFlags.allow = false; break;
    case Op::LDS_PR_M: PR = Read32(Rn); Rn += 4; m_intrFlags.allow = false; break;
    case Op::STC_GBR_M: Write32(Rn - 4, GBR); Rn -= 4; m_intrFlags.allow = false; break;
    case Op::STC_SR_M: Write32(Rn - 4, SR.u32); Rn -= 4; m_intrFlags.allow = false; break;
    case Op::STC_VBR_M: Write32(Rn - 4, VBR); Rn -= 4; m_intrFlags.allow = false; break;
    case Op::STS_MACH_M: Write32(Rn - 4, MAC.H); Rn -= 4; m_intrFlags.allow = false; break;
    case Op::STS_MACL_M: Write32(Rn - 4, MAC.L); Rn -= 4; m_intrFlags.allow = false; break;
    case Op::STS_PR_M: Write32(Rn - 4, PR); Rn -= 4; m_intrFlags.allow = false; break;
    case Op::ADD: Rn += Rm; break;
    case Op::ADD_I: Rn += static_cast<uint32>(imm); break;
    case Op::ADDC: {
        const uint64 sum = static_cast<uint64>(Rn) + Rm + SR.T;
        Rn = static_cast<uint32>(sum);
        SR.T = (sum >> 32) & 1;
        break;
    }
    case Op::ADDV: {
        const uint32 sum = Rn + Rm;
        SR.T = (~(Rn ^ Rm) & (Rn ^ sum)) >> 31;
        Rn = sum;
        break;
    }
    case Op::AND_R: Rn &= Rm; break;
    case Op::AND_I: R0 &= d8; break;
    case Op::AND_M: Write8(GBR + R0, static_cast<uint8>(Read8(GBR + R0) & d8)); break;
    case Op::NEG: Rn = 0u - Rm; break;
    case Op::NEGC: {
        const uint32 source = Rm;
        const uint32 result = 0u - source - SR.T;
        SR.T = (source != 0 || SR.T) ? 1 : 0;
        Rn = result;
        break;
    }
    case Op::NOT: Rn = ~Rm; break;
    case Op::OR_R: Rn |= Rm; break;
    case Op::OR_I: R0 |= d8; break;
    case Op::OR_M: Write8(GBR + R0, static_cast<uint8>(Read8(GBR + R0) | d8)); break;
    case Op::ROTCL: { const uint32 t = Rn >> 31; Rn = (Rn << 1) | SR.T; SR.T = t; break; }
    case Op::ROTCR: { const uint32 t = Rn & 1; Rn = (Rn >> 1) | (static_cast<uint32>(SR.T) << 31); SR.T = t; break; }
    case Op::ROTL: SR.T = Rn >> 31; Rn = (Rn << 1) | SR.T; break;
    case Op::ROTR: SR.T = Rn & 1; Rn = (Rn >> 1) | (static_cast<uint32>(SR.T) << 31); break;
    case Op::SHAL: case Op::SHLL: SR.T = Rn >> 31; Rn <<= 1; break;
    case Op::SHAR: SR.T = Rn & 1; Rn = static_cast<uint32>(static_cast<sint32>(Rn) >> 1); break;
    case Op::SHLR: SR.T = Rn & 1; Rn >>= 1; break;
    case Op::SHLL2: Rn <<= 2; break;
    case Op::SHLL8: Rn <<= 8; break;
    case Op::SHLL16: Rn <<= 16; break;
    case Op::SHLR2: Rn >>= 2; break;
    case Op::SHLR8: Rn >>= 8; break;
    case Op::SHLR16: Rn >>= 16; break;
    case Op::SUB: Rn -= Rm; break;
    case Op::SUBC: {
        const uint64 difference = static_cast<uint64>(Rn) - Rm - SR.T;
        Rn = static_cast<uint32>(difference);
        SR.T = (difference >> 32) & 1;
        break;
    }
    case Op::SUBV: {
        const uint32 difference = Rn - Rm;
        SR.T = ((Rn ^ Rm) & (Rn ^ difference)) >> 31;
        Rn = difference;
        break;
    }
    case Op::XOR_R: Rn ^= Rm; break;
    case Op::XOR_I: R0 ^= d8; break;
    case Op::XOR_M: Write8(GBR + R0, static_cast<uint8>(Read8(GBR + R0) ^ d8)); break;
    case Op::DT: Rn -= 1; SR.T = Rn == 0; break;
    case Op::CLRMAC: MAC.u64 = 0; break;
    case Op::MACW: {
        const sint32 a = static_cast<sint16>(Read16(Rn));
        Rn += 2;
        const sint32 b = static_cast<sint16>(Read16(Rm));
        Rm += 2;
        const sint64 product = static_cast<sint64>(a) * b;
        if (SR.S) {
            // (the sum saturates at 32 bits, with a mark in MACH)
            const sint64 sum = static_cast<sint64>(static_cast<sint32>(MAC.L)) + product;
            if (sum > INT32_MAX) {
                MAC.L = 0x7FFFFFFFu;
                MAC.H |= 1;
            } else if (sum < INT32_MIN) {
                MAC.L = 0x80000000u;
                MAC.H |= 1;
            } else {
                MAC.L = static_cast<uint32>(sum);
            }
        } else {
            MAC.u64 += static_cast<uint64>(product);
        }
        break;
    }
    case Op::MACL: {
        const sint64 a = static_cast<sint32>(Read32(Rn));
        Rn += 4;
        const sint64 b = static_cast<sint32>(Read32(Rm));
        Rm += 4;
        const sint64 product = a * b;
        if (SR.S) {
            // (the sum saturates at 48 bits)
            const __int128 sum = static_cast<__int128>(static_cast<sint64>(MAC.u64)) + product;
            const sint64 high = 0x00007FFFFFFFFFFFll, low = -0x0000800000000000ll;
            MAC.u64 = static_cast<uint64>(sum > high ? high : sum < low ? low : static_cast<sint64>(sum));
        } else {
            MAC.u64 += static_cast<uint64>(product);
        }
        break;
    }
    case Op::MUL: MAC.L = Rn * Rm; break;
    case Op::MULS: MAC.L = static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(Rn)) * static_cast<sint16>(Rm)); break;
    case Op::MULU: MAC.L = (Rn & 0xFFFF) * (Rm & 0xFFFF); break;
    case Op::DMULS: MAC.u64 = static_cast<uint64>(static_cast<sint64>(static_cast<sint32>(Rn)) * static_cast<sint32>(Rm)); break;
    case Op::DMULU: MAC.u64 = static_cast<uint64>(Rn) * Rm; break;
    case Op::DIV0S: SR.Q = Rn >> 31; SR.M = Rm >> 31; SR.T = SR.Q != SR.M; break;
    case Op::DIV0U: SR.Q = SR.M = SR.T = 0; break;
    case Op::DIV1: {
        // One step of a division: the dividend shifted left with T coming in, the divisor taken off or added
        const bool oldQ = SR.Q;
        bool q = Rn >> 31;
        Rn = (Rn << 1) | SR.T;
        const uint32 before = Rn;
        bool carry;
        if (oldQ == static_cast<bool>(SR.M)) {
            Rn -= Rm;
            carry = Rn > before;
        } else {
            Rn += Rm;
            carry = Rn < before;
        }
        q = (q != carry) != static_cast<bool>(SR.M) ? true : false;
        // (Q is the old top bit, turned over by a carry and by M)
        SR.Q = q;
        SR.T = SR.Q == SR.M;
        break;
    }
    case Op::CMP_EQ_I: SR.T = R0 == static_cast<uint32>(imm); break;
    case Op::CMP_EQ_R: SR.T = Rn == Rm; break;
    case Op::CMP_GE: SR.T = static_cast<sint32>(Rn) >= static_cast<sint32>(Rm); break;
    case Op::CMP_GT: SR.T = static_cast<sint32>(Rn) > static_cast<sint32>(Rm); break;
    case Op::CMP_HI: SR.T = Rn > Rm; break;
    case Op::CMP_HS: SR.T = Rn >= Rm; break;
    case Op::CMP_PL: SR.T = static_cast<sint32>(Rn) > 0; break;
    case Op::CMP_PZ: SR.T = static_cast<sint32>(Rn) >= 0; break;
    case Op::CMP_STR: {
        const uint32 x = Rn ^ Rm;
        SR.T = !(x & 0xFF000000u) || !(x & 0x00FF0000u) || !(x & 0x0000FF00u) || !(x & 0x000000FFu);
        break;
    }
    case Op::TAS: {
        const uint8 value = Read8(Rn);
        SR.T = value == 0;
        Write8(Rn, value | 0x80);
        break;
    }
    case Op::TST_R: SR.T = (Rn & Rm) == 0; break;
    case Op::TST_I: SR.T = (R0 & d8) == 0; break;
    case Op::TST_M: SR.T = (Read8(GBR + R0) & d8) == 0; break;
    case Op::BF:
        if (!SR.T) { next = pc + 4 + static_cast<uint32>(imm * 2); }
        break;
    case Op::BT:
        if (SR.T) { next = pc + 4 + static_cast<uint32>(imm * 2); }
        break;
    case Op::BFS:
        if (!SR.T) { delayed(pc + 4 + static_cast<uint32>(imm * 2)); }
        break;
    case Op::BTS:
        if (SR.T) { delayed(pc + 4 + static_cast<uint32>(imm * 2)); }
        break;
    case Op::BRA: delayed(pc + 4 + static_cast<uint32>((static_cast<sint32>(opcode << 20) >> 20) * 2)); break;
    case Op::BRAF: delayed(pc + 4 + Rn); break;
    case Op::BSR: {
        const uint32 target = pc + 4 + static_cast<uint32>((static_cast<sint32>(opcode << 20) >> 20) * 2);
        NoteCall(pc, target);
        PR = pc + 4;
        delayed(target);
        break;
    }
    case Op::BSRF: NoteCall(pc, pc + 4 + Rn); PR = pc + 4; delayed(pc + 4 + Rn); break;
    case Op::JMP: delayed(Rn); break;
    case Op::JSR: { const uint32 target = Rn; NoteCall(pc, target); PR = pc + 4; delayed(target); break; }
    case Op::TRAPA:
        EnterException(static_cast<uint8>(d8), pc + 2);
        jumped = true;
        break;
    case Op::RTE: {
        const uint32 target = Read32(R[15]);
        R[15] += 4;
        const uint32 sr = Read32(R[15]);
        R[15] += 4;
        writeSR(sr);
        delayed(target);
        break;
    }
    case Op::RTS: delayed(PR); break;
    default: {
        static unsigned reports = 8;
        if (reports) {
            reports--;
            fprintf(stderr, "%s CPU: illegal instruction %04X at %08X\n", m_master ? "master" : "slave", opcode, pc);
        }
        EnterException(inSlot ? 6 : 4, pc);
        jumped = true;
        break;
    }
    }
    if (!jumped) {
        PC = inSlot ? slotTarget : next;
    }
}

} // namespace satrecomp::machine
