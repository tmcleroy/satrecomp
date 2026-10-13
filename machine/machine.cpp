// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "machine.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace satrecomp::machine {

namespace {
    uint16 Get16(const uint8 *p) {
        return static_cast<uint16>((p[0] << 8) | p[1]);
    }
    void Put16(uint8 *p, uint16 v) {
        p[0] = static_cast<uint8>(v >> 8);
        p[1] = static_cast<uint8>(v);
    }
    // With 24-bit colours, colour RAM keeps the two words of a colour 0x800 bytes apart (video/renderer.h)
    uint32 CramAddress(uint32 address, bool wide) {
        address &= 0xFFF;
        return wide ? ((address & 2) << 10) | ((address >> 1) & 0x7FE) | (address & 1) : address;
    }
    constexpr uint32 kSampleRate = 44100;
    constexpr uint32 kSoundCpuCyclesPerSample = 256;
} // namespace

Machine::Machine()
    : masterSH2(*this, true)
    , slaveSH2(*this, false)
    , m_ramHigh(kRamSize)
    , m_ramLow(kRamSize)
    , m_rom(kRomSize, 0xFF)
    , m_soundRam(0x80000)
    , m_vram1(0x80000)
    , m_vram2(0x80000)
    , m_cram(0x1000) {
    ramHigh = m_ramHigh.data();
    ramLow = m_ramLow.data();
    rom = m_rom.data();
    m_fb[0].resize(0x40000);
    m_fb[1].resize(0x40000);
    video::Memory memory;
    memory.vram1 = m_vram1.data();
    memory.fb[0] = m_fb[0].data();
    memory.fb[1] = m_fb[1].data();
    memory.vram2 = m_vram2.data();
    memory.cram = m_cram.data();
    m_video = std::make_unique<video::Renderer>(memory);
    m_sound = std::make_unique<sound::Chip>(m_soundRam.data());
    m_sound->onSoundCpuInterrupt = [this](uint8 level) { m_soundCpu.SetInterruptLevel(level); };
    m_sound->onMainInterrupt = [this](bool level) {
        // (the sound request line follows the chip's level)
        const bool raised = m_scu.status & (1u << 6);
        if (raised != level) {
            m_scu.status = (m_scu.status & ~(1u << 6)) | (static_cast<uint32>(level) << 6);
            ScuUpdateInterrupt();
            if (level) {
                ScuTriggerDma(5);
            }
        }
    };
}

Machine::~Machine() = default;

void Machine::LoadRom(const uint8 *data, size_t size) {
    std::copy_n(data, std::min(size, m_rom.size()), m_rom.begin());
}

void Machine::Reset(bool hard) {
    if (hard) {
        std::fill(m_ramHigh.begin(), m_ramHigh.end(), 0);
        std::fill(m_ramLow.begin(), m_ramLow.end(), 0);
        std::fill(m_soundRam.begin(), m_soundRam.end(), 0);
        std::fill(m_vram2.begin(), m_vram2.end(), 0);
        std::fill(m_cram.begin(), m_cram.end(), 0);
        std::fill(m_fb[0].begin(), m_fb[0].end(), 0);
        std::fill(m_fb[1].begin(), m_fb[1].end(), 0);
        // (VDP1 VRAM as it is at power on: a pattern that reads as a table of "end" commands)
        for (size_t i = 0; i < m_vram1.size(); i++) {
            m_vram1[i] = (i & 0x1F) == 0 ? 0x80 : (i & 0x1F) == 1 ? 0x00 : (i & 2) ? 0x55 : 0xAA;
        }
    }
    SetClock(false);
    ScuReset();
    m_smpc = {};
    VdpReset();
    m_sound->Reset();
    m_soundCpu.Reset(true);
    m_soundCpuEnabled = false;
    m_soundCpuSpillover = 0;
    CdReset();
    masterSH2.Reset(hard);
    slaveSH2.Reset(hard);
    slaveEnabled = false;
    m_masterSpillover = m_slaveSpillover = 0;
    m_sampleFraction = 0;
    m_sampleDue = m_now + m_clock / kSampleRate;
}

void Machine::SetClock(bool dots352) {
    m_clock352 = dots352;
    m_clock = dots352 ? 28636364 : 26846591; // (39.375 MHz x 8/11, or x 120/176)
}

void Machine::RunFrame() {
    m_frameDone = false;
    while (!m_frameDone) {
        Run();
    }
}

// The CPUs up to the next thing due, then that. The two CPUs take turns: each runs the whole stretch unless
// exactly one of them is only waiting (for the other): then they alternate in short slices, so that the one
// waiting sees what it waits for soon.
void Machine::Run() {
    uint64 due = std::min(m_vdp.phaseDue, m_sampleDue);
    for (const uint64 other : {m_smpc.commandDue, m_scu.timer1Due, m_vdp.drawEndDue, m_cd.periodicDue}) {
        if (other) {
            due = std::min(due, other);
        }
    }
    const uint64 cycles = due > m_now ? due - m_now : 0;
    uint64 executed = m_masterSpillover;
    m_masterSpillover = 0;
    if (cycles > 0) {
        if (slaveEnabled) {
            uint64 slaveCycles = m_slaveSpillover;
            do {
                const bool masterWaits = masterSH2.RecompWaiting(), slaveWaits = slaveSH2.RecompWaiting();
                const uint64 step = masterWaits == slaveWaits && !m_fineCycles ? cycles : 32;
                const uint64 target = std::min(executed + step, cycles);
                const uint64 before = executed;
                executed = masterSH2.Advance(target, executed);
                slaveCycles = slaveSH2.Advance(executed, slaveCycles);
                m_fineCycles -= std::min<uint64>(m_fineCycles, executed - before);
            } while (executed < cycles);
            while (slaveCycles < executed) { // (a slave that let the master go first catches up)
                slaveCycles = slaveSH2.Advance(executed, slaveCycles);
            }
            m_slaveSpillover = slaveCycles - executed;
        } else {
            do {
                executed = masterSH2.Advance(cycles, executed);
            } while (executed < cycles);
        }
    }
    m_now += executed;

    while (m_vdp.phaseDue <= m_now) {
        VdpPhase();
    }
    while (m_sampleDue <= m_now) {
        SoundSample();
    }
    if (m_smpc.commandDue && m_smpc.commandDue <= m_now) {
        m_smpc.commandDue = 0;
        SmpcCommand();
    }
    if (m_scu.timer1Due && m_scu.timer1Due <= m_now) {
        m_scu.timer1Due = 0;
        if (!m_scu.timer1Mode || m_scu.timer0Counter == m_scu.timer0Compare) {
            ScuRaise(4);
            ScuTriggerDma(4);
        }
    }
    if (m_vdp.drawEndDue && m_vdp.drawEndDue <= m_now) {
        m_vdp.drawEndDue = 0;
        m_vdp.frameEnded = true;
        ScuRaise(13);
        ScuTriggerDma(6);
    }
    if (m_cd.periodicDue && m_cd.periodicDue <= m_now) {
        CdPeriodic();
    }
}

// ---------------------------------------------------------------------------------------------------------
// The bus

uint8 Machine::BusRead8(uint32 a) {
    switch (a >> 20) {
    case 0x00: return m_rom[a & 0x7FFFF];
    case 0x01: return a < 0x00180000 ? SmpcRead(a & 0x7F) : 0xFF;
    case 0x02: return m_ramLow[a & 0xFFFFF];
    case 0x5A: return m_soundRam[a & 0x7FFFF];
    case 0x5B: return static_cast<uint8>(m_sound->Read(a & 0xFFF, false));
    case 0x5C: return a < 0x05C80000 ? m_vram1[a & 0x7FFFF] : m_fb[m_video->DisplayFramebuffer() ^ 1][a & 0x3FFFF];
    case 0x5E: return m_vram2[a & 0x7FFFF];
    case 0x5F:
        if (a < 0x05F80000) {
            return m_cram[CramAddress(a, m_video->Regs2().vramControl.colorRAMMode >= 2)];
        }
        break;
    default:
        if (a >= 0x06000000) {
            return m_ramHigh[a & 0xFFFFF];
        }
        break;
    }
    // (the rest are registers of a word or more: a byte of the word)
    const uint16 word = BusRead16(a & ~1u);
    return static_cast<uint8>((a & 1) ? word : word >> 8);
}

uint16 Machine::BusRead16(uint32 a) {
    switch (a >> 20) {
    case 0x00: return Get16(&m_rom[a & 0x7FFFE]);
    case 0x01: return a < 0x00180000 ? static_cast<uint16>(0xFF00 | SmpcRead((a & 0x7F) | 1)) : 0xFFFF;
    case 0x02: return Get16(&m_ramLow[a & 0xFFFFE]);
    case 0x58: return CdRead(a & 0xFFFFF);
    case 0x5A: return Get16(&m_soundRam[a & 0x7FFFE]);
    case 0x5B: return m_sound->Read(a & 0xFFE, true);
    case 0x5C: return a < 0x05C80000 ? Get16(&m_vram1[a & 0x7FFFE]) : Get16(&m_fb[m_video->DisplayFramebuffer() ^ 1][a & 0x3FFFE]);
    case 0x5D: return Vdp1ReadRegister(a & 0x1E);
    case 0x5E: return Get16(&m_vram2[a & 0x7FFFE]);
    case 0x5F:
        if (a < 0x05F80000) {
            return Get16(&m_cram[CramAddress(a & ~1u, m_video->Regs2().vramControl.colorRAMMode >= 2)]);
        }
        if (a < 0x05FE0000) {
            return Vdp2ReadRegister(a & 0x1FE);
        }
        return static_cast<uint16>((a & 2) ? ScuRead(a & 0xFC) : ScuRead(a & 0xFC) >> 16);
    default:
        if (a >= 0x06000000) {
            return Get16(&m_ramHigh[a & 0xFFFFE]);
        }
        return 0xFFFF; // (nothing there: the cartridge slot, unused addresses)
    }
}

uint32 Machine::BusRead32(uint32 a) {
    if (a >= 0x05FE0000 && a < 0x05FF0000) {
        return ScuRead(a & 0xFC);
    }
    const uint32 high = BusRead16(a);
    return (high << 16) | BusRead16(a + 2);
}

void Machine::BusWrite8(uint32 a, uint8 v) {
    switch (a >> 20) {
    case 0x01:
        if (a < 0x00180000) {
            SmpcWrite(a & 0x7F, v);
        }
        return;
    case 0x02:
        m_ramLow[a & 0xFFFFF] = v;
        NoteWrite(&m_ramLow[a & 0xFFFFF]);
        return;
    case 0x5A: m_soundRam[a & 0x7FFFF] = v; return;
    case 0x5B: m_sound->Write(a & 0xFFF, v, false); return;
    case 0x5C:
        if (a < 0x05C80000) {
            m_vram1[a & 0x7FFFF] = v;
        } else {
            m_fb[m_video->DisplayFramebuffer() ^ 1][a & 0x3FFFF] = v;
        }
        return;
    case 0x5E: m_vram2[a & 0x7FFFF] = v; return;
    case 0x5F:
        if (a < 0x05F80000) {
            m_cram[CramAddress(a, m_video->Regs2().vramControl.colorRAMMode >= 2)] = v;
            return;
        }
        break;
    default:
        if (a >= 0x06000000) {
            m_ramHigh[a & 0xFFFFF] = v;
            NoteWrite(&m_ramHigh[a & 0xFFFFF]);
            return;
        }
        break;
    }
    if (a >= 0x01000000 && a < 0x02000000) {
        BusWrite16(a & ~1u, v);
        return;
    }
    // (a byte of a register of a word: the other byte stays)
    if ((a >> 20) == 0x5D || (a >> 20) == 0x5F || (a >> 20) == 0x58) {
        const uint16 word = BusRead16(a & ~1u);
        BusWrite16(a & ~1u, static_cast<uint16>((a & 1) ? (word & 0xFF00) | v : (word & 0x00FF) | (v << 8)));
    }
}

void Machine::BusWrite16(uint32 a, uint16 v) {
    switch (a >> 20) {
    case 0x01:
        if (a < 0x00180000) {
            SmpcWrite((a & 0x7F) | 1, static_cast<uint8>(v));
        }
        return;
    case 0x02:
        Put16(&m_ramLow[a & 0xFFFFE], v);
        NoteWrite(&m_ramLow[a & 0xFFFFE]);
        return;
    case 0x58: CdWrite(a & 0xFFFFF, v); return;
    case 0x5A: Put16(&m_soundRam[a & 0x7FFFE], v); return;
    case 0x5B: m_sound->Write(a & 0xFFE, v, true); return;
    case 0x5C:
        if (a < 0x05C80000) {
            Put16(&m_vram1[a & 0x7FFFE], v);
        } else {
            Put16(&m_fb[m_video->DisplayFramebuffer() ^ 1][a & 0x3FFFE], v);
        }
        return;
    case 0x5D: Vdp1WriteRegister(a & 0x1E, v); return;
    case 0x5E: Put16(&m_vram2[a & 0x7FFFE], v); return;
    case 0x5F:
        if (a < 0x05F80000) {
            Put16(&m_cram[CramAddress(a & ~1u, m_video->Regs2().vramControl.colorRAMMode >= 2)], v);
        } else if (a < 0x05FE0000) {
            Vdp2WriteRegister(a & 0x1FE, v);
        } else {
            const uint32 before = ScuRead(a & 0xFC);
            ScuWrite(a & 0xFC, (a & 2) ? (before & 0xFFFF0000u) | v : (before & 0xFFFFu) | (static_cast<uint32>(v) << 16));
        }
        return;
    default: break;
    }
    if (a >= 0x06000000) {
        Put16(&m_ramHigh[a & 0xFFFFE], v);
        NoteWrite(&m_ramHigh[a & 0xFFFFE]);
    } else if (a >= 0x01000000 && a < 0x01800000) {
        // A write here pulses the slave CPU's timer input: how the master tells it to start on something.
        // On the hardware the slave reacts while the master goes on; here the master's turn ends and the two
        // alternate in short slices for a while.
        slaveSH2.TriggerInputCapture();
        masterSH2.RequestYield();
        m_fineCycles = kFineCyclesAfterSignal;
    } else if (a >= 0x01800000 && a < 0x02000000) {
        masterSH2.TriggerInputCapture(); // (... and here the slave signals the master)
        slaveSH2.RequestYield();
        m_fineCycles = kFineCyclesAfterSignal;
    }
}

void Machine::BusWrite32(uint32 a, uint32 v) {
    if (a >= 0x05FE0000 && a < 0x05FF0000) {
        ScuWrite(a & 0xFC, v);
        return;
    }
    if (a >= 0x01000000 && a < 0x02000000) {
        BusWrite16(a, static_cast<uint16>(v));
        return;
    }
    BusWrite16(a, static_cast<uint16>(v >> 16));
    BusWrite16(a + 2, static_cast<uint16>(v));
}

// ---------------------------------------------------------------------------------------------------------
// System control unit: collects the hardware's interrupts for the master CPU, and has DMA and two timers

void Machine::ScuReset() {
    m_scu = {};
    masterSH2.SetExternalInterrupt(0, 0);
    slaveSH2.SetExternalInterrupt(0, 0);
}

void Machine::ScuRaise(unsigned interrupt) {
    if (!(m_scu.status & (1u << interrupt))) {
        m_scu.status |= 1u << interrupt;
        ScuUpdateInterrupt();
    }
}

// The interrupt offered to the master CPU: the lowest numbered one that is raised and not masked
void Machine::ScuUpdateInterrupt() {
    if (m_scu.pendingLevel > 0) {
        return;
    }
    const uint32 bits = m_scu.status & ~m_scu.mask & 0x3FFF;
    if (!bits) {
        return;
    }
    static constexpr uint8 kLevels[14] = {15, 14, 13, 12, 11, 10, 9, 8, 8, 6, 6, 5, 3, 2};
    const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
    m_scu.pendingLevel = kLevels[index];
    m_scu.status &= ~(1u << index);
    masterSH2.SetExternalInterrupt(kLevels[index], static_cast<uint8>(0x40 + index));
}

void Machine::AcknowledgeExternalInterrupt(SH2 &cpu) {
    if (&cpu == &masterSH2) {
        // (taking an interrupt masks them all: the handler says which may come next)
        if (m_scu.pendingLevel > 0) {
            m_scu.pendingLevel = 0;
            m_scu.mask = 0xBFFF;
        }
    }
    cpu.SetExternalInterrupt(0, 0);
}

uint32 Machine::ScuRead(uint32 address) {
    switch (address) {
    case 0xA0: return m_scu.mask;
    case 0xA4: return m_scu.status;
    case 0xC8: return 4; // (version)
    case 0x7C: return 0; // (DMA status: nothing running)
    default: return m_scu.registers[address >> 2];
    }
}

void Machine::ScuWrite(uint32 address, uint32 value) {
    m_scu.registers[address >> 2] = value;
    if (address < 0x60) {
        DmaLevel &dma = m_scu.dma[address >> 5];
        switch (address & 0x1C) {
        case 0x00: dma.source = value & 0x07FFFFFF; break;
        case 0x04: dma.target = value & 0x07FFFFFF; break;
        case 0x08: dma.count = value & (address < 0x20 ? 0xFFFFF : 0xFFF); break;
        case 0x0C:
            dma.sourceAdd = (value & 0x100) ? 4 : 0;
            dma.targetAdd = (value & 7) ? 1u << (value & 7) : 0;
            break;
        case 0x10:
            dma.enabled = value & 0x100;
            if (dma.enabled && dma.trigger == 7 && (value & 1)) {
                ScuRunDma(address >> 5);
            }
            break;
        case 0x14:
            dma.indirect = value & 0x01000000;
            dma.updateSource = value & 0x00010000;
            dma.updateTarget = value & 0x00000100;
            dma.trigger = value & 7;
            break;
        default: break;
        }
        return;
    }
    switch (address) {
    case 0x90: m_scu.timer0Compare = value & 0x3FF; break;
    case 0x94: m_scu.timer1Reload = value & 0x1FF; break;
    case 0x98:
        m_scu.timerEnable = value & 1;
        m_scu.timer1Mode = value & 0x100;
        break;
    case 0xA0:
        m_scu.mask = value & 0xBFFF;
        ScuUpdateInterrupt();
        break;
    case 0xA4:
        m_scu.status &= value; // (writing 0 to a bit takes the interrupt back)
        break;
    default: break;
    }
}

void Machine::ScuTriggerDma(uint8 trigger) {
    for (unsigned level = 0; level < 3; level++) {
        if (m_scu.dma[level].enabled && m_scu.dma[level].trigger == trigger) {
            ScuRunDma(level);
        }
    }
}

// A DMA transfer, all at once: long words from the source; to the video and sound chips they go as words
// (the address stepping by the amount set), elsewhere as long words. Indirect: a table of transfers.
void Machine::ScuRunDma(unsigned level) {
    DmaLevel &dma = m_scu.dma[level];
    uint32 table = dma.target;
    bool last = !dma.indirect;
    do {
        uint32 count, target, source;
        if (dma.indirect) {
            count = BusRead32(table);
            target = BusRead32(table + 4) & 0x07FFFFFF;
            source = BusRead32(table + 8);
            table += 12;
            last = source & 0x80000000u;
            source &= 0x07FFFFFF;
            count &= 0xFFFFF;
        } else {
            count = dma.count;
            target = dma.target;
            source = dma.source;
        }
        if (count == 0) {
            count = level == 0 ? 0x100000 : 0x1000;
        }
        // (debugging aid, SATRECOMP_DMA_TRACE=1: the first transfers of each kind)
        static const bool trace = getenv("SATRECOMP_DMA_TRACE") != nullptr;
        static unsigned traced = 0;
        if (trace && traced < 400 && (m_now / m_clock) >= 60) {
            traced++;
            fprintf(stderr, "dma%u %s %08X -> %08X, %X bytes (+%u/+%u) trigger %u\n", level, dma.indirect ? "indirect" : "direct", source,
                    target, count, dma.sourceAdd, dma.targetAdd, dma.trigger);
        }
        const bool toChips = target >= 0x05A00000 && target < 0x05FE0000;
        const uint32 sourceAdd = dma.indirect ? 4 : dma.sourceAdd;
        uint32 buffer = 0, held = 0; // bytes read from the source and not yet written
        auto next = [&]() -> uint8 {
            if (held == 0) {
                buffer = BusRead32(source & ~3u);
                // (a source that does not start on a long word: its first bytes are skipped)
                held = 4 - (source & 3);
                buffer <<= (source & 3) * 8;
                source = (source & ~3u) + sourceAdd;
            }
            const uint8 byte = static_cast<uint8>(buffer >> 24);
            buffer <<= 8;
            held--;
            return byte;
        };
        while (count > 0) {
            if (toChips) {
                if (count >= 2) {
                    const uint16 high = next();
                    BusWrite16(target, static_cast<uint16>((high << 8) | next()));
                    count -= 2;
                } else {
                    BusWrite8(target, next());
                    count--;
                }
                target += dma.targetAdd;
            } else if (count >= 4 && !(target & 3)) {
                uint32 value = 0;
                for (int i = 0; i < 4; i++) {
                    value = (value << 8) | next();
                }
                BusWrite32(target, value);
                count -= 4;
                target += dma.targetAdd ? 4 : 0;
            } else {
                BusWrite8(target, next());
                count--;
                target += dma.targetAdd ? 1 : 0;
            }
        }
        if (!dma.indirect) {
            if (dma.updateSource) {
                dma.source = source & 0x07FFFFFF;
            }
            if (dma.updateTarget) {
                dma.target = target & 0x07FFFFFF;
            }
        }
    } while (!last);
    if (dma.indirect && dma.updateTarget) {
        dma.target = table & 0x07FFFFFF;
    }
    ++g_activity;
    ScuRaise(11 - level); // DMA end
}

// ---------------------------------------------------------------------------------------------------------
// System manager (SMPC): turns the slave CPU and the sound CPU on and off, changes the clock, keeps the
// time of day, and reads the control pads

uint8 Machine::SmpcRead(uint32 address) {
    if (address >= 0x21 && address <= 0x5F) {
        return m_smpc.OREG[(address - 0x21) >> 1];
    }
    switch (address) {
    case 0x61: return m_smpc.SR;
    case 0x63: return m_smpc.SF ? 1 : 0;
    case 0x75:
    case 0x77: return 0xFF; // (the ports read directly: nothing pressed)
    default: return 0xFF;
    }
}

void Machine::SmpcWrite(uint32 address, uint8 value) {
    if (address >= 0x01 && address <= 0x0D) {
        m_smpc.IREG[(address - 1) >> 1] = value;
        if (address == 0x01 && m_smpc.intbackInProgress) {
            // (the game asks for the rest of the peripheral data, or says it has enough)
            if (value & 0x80) {
                m_smpc.SF = true;
                if (!m_smpc.optimize) {
                    m_smpc.commandDue = m_now + 1000;
                }
            } else if (value & 0x40) {
                m_smpc.intbackInProgress = false;
            }
        }
        return;
    }
    switch (address) {
    case 0x1F:
        if (m_smpc.commandDue) {
            return; // (one is running)
        }
        m_smpc.command = value;
        smpcCommands[value]++;
        // (a clock change or reset takes a while)
        m_smpc.commandDue = m_now + (value == 0x0D || value == 0x0E || value == 0x0F ? 200000 : 240);
        break;
    case 0x63: m_smpc.SF = true; break;
    case 0x75: m_smpc.PDR[0] = value; break;
    case 0x77: m_smpc.PDR[1] = value; break;
    case 0x79: m_smpc.DDR[0] = value; break;
    case 0x7B: m_smpc.DDR[1] = value; break;
    case 0x7D: m_smpc.IOSEL = value; break;
    case 0x7F: m_smpc.EXLE = value; break;
    default: break;
    }
}

void Machine::SmpcCommand() {
    auto &s = m_smpc;
    s.OREG[31] = s.command;
    switch (s.command) {
    case 0x02: // SSHON (a slave CPU that is already running goes on)
        if (!slaveEnabled) {
            slaveEnabled = true;
            slaveSH2.Reset(true);
            m_slaveSpillover = 0;
        }
        break;
    case 0x03: slaveEnabled = false; break; // SSHOFF
    case 0x06: // SNDON
        if (!m_soundCpuEnabled) {
            m_soundCpuEnabled = true;
            m_soundCpu.Reset(true);
            m_soundCpuSpillover = 0;
        }
        break;
    case 0x07: m_soundCpuEnabled = false; break; // SNDOFF
    case 0x0D: // SYSRES
        Reset(false);
        return;
    case 0x0E: // CKCHG352, CKCHG320: the video and sound chips and the SCU start over, the slave CPU is turned
    case 0x0F: // off and the master told by an NMI
        ScuReset();
        VdpReset();
        m_sound->Reset();
        slaveEnabled = false;
        SetClock(s.command == 0x0E);
        masterSH2.SetNMI();
        break;
    case 0x10: // INTBACK: the system's status and/or the peripherals' data
        if (s.intbackInProgress) {
            SmpcPeripheralReport();
        } else {
            const bool status = s.IREG[0] == 0x01;
            s.optimize = !(s.IREG[1] & 0x02);
            s.getPeripheralData = s.IREG[1] & 0x08;
            s.portMode[0] = (s.IREG[1] >> 4) & 3;
            s.portMode[1] = (s.IREG[1] >> 6) & 3;
            s.intbackInProgress = s.getPeripheralData;
            if (status) {
                SmpcStatusReport();
            } else if (s.getPeripheralData) {
                SmpcPeripheralReport();
            }
        }
        s.SF = false;
        ScuRaise(7);
        return;
    case 0x17: // SETSMEM
        for (int i = 0; i < 4; i++) {
            s.SMEM[i] = s.IREG[i];
        }
        break;
    case 0x18: masterSH2.SetNMI(); break; // NMIREQ
    case 0x19: s.resetDisable = false; break;
    case 0x1A: s.resetDisable = true; break;
    default: break;
    }
    s.SF = false;
}

void Machine::SmpcStatusReport() {
    auto &s = m_smpc;
    s.SR = static_cast<uint8>(0x40 | (s.getPeripheralData ? 0x20 : 0) | (s.portMode[1] << 2) | s.portMode[0]);
    auto bcd = [](int v) { return static_cast<uint8>(((v / 10) << 4) | (v % 10)); };
    const time_t now = static_cast<time_t>(readClock ? readClock() : 820454400); // (1 January 1996)
    struct tm t {};
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    s.OREG[0] = static_cast<uint8>(0x80 | (s.resetDisable << 6));
    s.OREG[1] = bcd((t.tm_year + 1900) / 100);
    s.OREG[2] = bcd((t.tm_year + 1900) % 100);
    s.OREG[3] = static_cast<uint8>((t.tm_wday << 4) | (t.tm_mon + 1));
    s.OREG[4] = bcd(t.tm_mday);
    s.OREG[5] = bcd(t.tm_hour);
    s.OREG[6] = bcd(t.tm_min);
    s.OREG[7] = bcd(t.tm_sec);
    s.OREG[8] = 0;    // no cartridge
    s.OREG[9] = 0x04; // area: North America
    s.OREG[10] = static_cast<uint8>(0x34 | (m_clock352 << 6));
    s.OREG[11] = 0;
    for (int i = 0; i < 4; i++) {
        s.OREG[12 + i] = s.SMEM[i];
    }
}

// The peripherals' data: for each port, what is connected (one control pad) and its buttons
void Machine::SmpcPeripheralReport() {
    auto &s = m_smpc;
    const bool first = s.reportOffset == 0;
    if (first) {
        s.reportSize = 0;
        for (int player = 0; player < 2; player++) {
            const uint16 buttons = static_cast<uint16>(~(readPad ? readPad(player) : 0) | 0x0007); // (0 = pressed)
            s.report[s.reportSize++] = 0xF1; // one peripheral, connected directly
            s.report[s.reportSize++] = 0x02; // a digital pad: two bytes
            s.report[s.reportSize++] = static_cast<uint8>(buttons >> 8);
            s.report[s.reportSize++] = static_cast<uint8>(buttons);
        }
    }
    const size_t length = std::min<size_t>(32, s.reportSize - s.reportOffset);
    std::fill(std::begin(s.OREG), std::end(s.OREG), 0xFF);
    std::copy_n(s.report + s.reportOffset, length, s.OREG);
    s.reportOffset += static_cast<uint32>(length);
    if (s.reportOffset == s.reportSize) {
        s.intbackInProgress = false;
        s.reportOffset = 0;
        s.optimize = false;
    }
    s.SR = static_cast<uint8>(0x80 | (first ? 0x40 : 0) | (s.intbackInProgress ? 0x20 : 0) | (s.portMode[1] << 2) | s.portMode[0]);
}

// ---------------------------------------------------------------------------------------------------------
// Sound: a sample of the chip's, 44,100 times a second, and the sound CPU's share of time before it

void Machine::SoundSample() {
    if (m_soundCpuEnabled) {
        uint64 cycles = m_soundCpuSpillover;
        while (cycles < kSoundCpuCyclesPerSample) {
            cycles += m_soundCpu.Step();
        }
        m_soundCpuSpillover = cycles - kSoundCpuCyclesPerSample;
    }
    for (uint32 i = 0; i < 32; i++) {
        m_sound->Step(i);
    }
    const uint64 total = m_clock + m_sampleFraction;
    m_sampleDue += total / kSampleRate;
    m_sampleFraction = total % kSampleRate;
}

uint8 Machine::Read8(uint32 address) {
    if (address < 0x80000) {
        return m_soundRam[address];
    }
    return address >= 0x100000 && address < 0x200000 ? static_cast<uint8>(m_sound->Read(address & 0xFFF, false)) : 0;
}
uint16 Machine::Read16(uint32 address) {
    if (address < 0x80000) {
        return Get16(&m_soundRam[address]);
    }
    return address >= 0x100000 && address < 0x200000 ? m_sound->Read(address & 0xFFF, true) : 0;
}
void Machine::Write8(uint32 address, uint8 value) {
    if (address < 0x80000) {
        m_soundRam[address] = value;
    } else if (address >= 0x100000 && address < 0x200000) {
        m_sound->Write(address & 0xFFF, value, false);
    }
}
void Machine::Write16(uint32 address, uint16 value) {
    if (address < 0x80000) {
        Put16(&m_soundRam[address], value);
    } else if (address >= 0x100000 && address < 0x200000) {
        m_sound->Write(address & 0xFFF, value, true);
    }
}

} // namespace satrecomp::machine
