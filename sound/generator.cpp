// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "generator.h"

#include "../runtime/m68k_access.h"
#include "m68k.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace satrecomp::sound {

using namespace ymir::scsp;

void Generator::Install(SCSP &scsp, bool check) {
    m_scsp = &scsp;
    m_check = check;
    m_chip = std::make_unique<Chip>(scsp.m_WRAM.data());
    m_chip->emit = !check; // (in a check Ymir's generation sends the samples out)
    m_chip->linearInterpolation = scsp.m_interpMode == ymir::core::config::audio::SampleInterpolationMode::Linear;
    m_chip->onSample = [this](sint16 left, sint16 right) { m_scsp->m_cbOutputSample(left, right); };

    SCSP::SatrecompHooks hooks;
    hooks.context = this;
    hooks.write = [](void *context, uint32 address, uint16 value, bool word) {
        auto &self = *static_cast<Generator *>(context);
        if (self.m_logWrites) {
            self.m_writeLog.push_back({address, value, word});
        }
        self.m_chip->Write(address, value, word);
    };
    hooks.read = [](void *context, uint32 address, bool word, uint16 &value) {
        // What the sample generation keeps is here: the monitored voice, the sound stack, the DSP's memories
        auto &self = *static_cast<Generator *>(context);
        if (self.m_check || !((address & ~1u) == 0x408 || (address >= 0x600 && address < 0x680) || (address >= 0xC00 && address < 0xEE4))) {
            return false;
        }
        value = self.m_chip->Read(address, word);
        return true;
    };
    hooks.reset = [](void *context) { static_cast<Generator *>(context)->m_chip->Reset(); };
    hooks.cdda = [](void *context, const uint8 *data) { return static_cast<Generator *>(context)->m_chip->ReceiveCDDA(data); };
    scsp.SetSatrecompHooks(hooks);
    scsp.SetSlotStepHandler(
        [](void *context, uint32 i) {
            auto &self = *static_cast<Generator *>(context);
            if (self.m_check) {
                self.CheckedStep(i);
            } else {
                self.m_chip->Step(i);
            }
        },
        this);
}

bool Generator::Differs(const char *what, long long theirs, long long mine) {
    if (theirs == mine) {
        return false;
    }
    if (m_reportsLeft && !m_quiet) {
        fprintf(stderr, " %s %lld/%lld", what, theirs, mine);
    }
    return true;
}

// Ymir's chip and this one, each from its own state, which should be the same before and after
void Generator::CheckedStep(uint32 i) {
    SCSP &ref = *m_scsp;
    Chip &chip = *m_chip;
    bool different = false;
#define SAME(WHAT, THEIRS, MINE) different |= Differs(WHAT, static_cast<long long>(THEIRS), static_cast<long long>(MINE))
    // What the registers, the timers and the end of the sample before left
    SAME("sample", ref.m_sampleCounter.load(), chip.m_sampleCounter);
    SAME("MVOL", ref.m_masterVolume, chip.m_masterVolume);
    SAME("DAC18B", ref.m_dac18Bits, chip.m_dac18Bits);
    SAME("MSLC", ref.m_monitorSlotCall, chip.m_monitorSlotCall);
    SAME("SCIEB", ref.m_m68kEnabledInterrupts, chip.m_m68kEnabledInterrupts);
    SAME("SCIPD", ref.m_m68kPendingInterrupts, chip.m_m68kPendingInterrupts);
    SAME("MCIEB", ref.m_scuEnabledInterrupts, chip.m_scuEnabledInterrupts);
    SAME("MCIPD", ref.m_scuPendingInterrupts, chip.m_scuPendingInterrupts);
    for (uint32 n = 0; n < 3; n++) {
        SAME("SCILV", ref.m_m68kInterruptLevels[n], chip.m_m68kInterruptLevels[n]);
        SAME("timer", ref.m_timers[n].counter, chip.m_timers[n].counter);
        SAME("timer reload", ref.m_timers[n].reload, chip.m_timers[n].reload);
        SAME("timer reload due", ref.m_timers[n].doReload, chip.m_timers[n].doReload);
        SAME("timer interval", ref.m_timers[n].incrementInterval, chip.m_timers[n].incrementInterval);
    }
    SAME("DMA address", ref.m_dmaMemAddress, chip.m_dmaMemAddress);
    SAME("DMA register", ref.m_dmaRegAddress, chip.m_dmaRegAddress);
    SAME("DMA length", ref.m_dmaXferLength, chip.m_dmaXferLength);
    SAME("KYONEX", ref.m_kyonex, chip.m_kyonex);

    ref.ReferenceSlotStep(i); // (this one sends the sample out)
    chip.Step(i);

    for (uint32 n = 0; n < 7; n++) {
        const ymir::scsp::Slot &a = ref.m_slots[(i - n) & 31];
        const sound::Slot &b = chip.m_slots[(i - n) & 31];
#define FIELD(F) SAME("slot " #F, a.F, b.F)
        FIELD(index); FIELD(startAddress); FIELD(loopStartAddress); FIELD(loopEndAddress); FIELD(pcm8Bit); FIELD(keyOnBit);
        FIELD(loopControl); FIELD(sampleXOR); FIELD(soundSource); FIELD(attackRate); FIELD(decay1Rate); FIELD(decay2Rate);
        FIELD(releaseRate); FIELD(decayLevel); FIELD(keyRateScaling); FIELD(egHold); FIELD(loopStartLink); FIELD(egBypass);
        FIELD(modLevel); FIELD(modXSelect); FIELD(modYSelect); FIELD(stackWriteInhibit); FIELD(totalLevel); FIELD(soundDirect);
        FIELD(octave); FIELD(freqNumSwitch); FIELD(maskMode); FIELD(mask); FIELD(lfoReset); FIELD(lfofRaw); FIELD(lfoStepInterval);
        FIELD(ampLFOSens); FIELD(pitchLFOSens); FIELD(ampLFOWaveform); FIELD(pitchLFOWaveform); FIELD(inputMixingLevel);
        FIELD(inputSelect); FIELD(directSendLevel); FIELD(directPan); FIELD(effectSendLevel); FIELD(effectPan); FIELD(active);
        FIELD(egState); FIELD(egLevel); FIELD(currEGRate); FIELD(currEGLevel); FIELD(egAttackBug); FIELD(currSample);
        FIELD(currPhase); FIELD(nextPhase); FIELD(modXSample); FIELD(modYSample); FIELD(modulation); FIELD(reverse);
        FIELD(crossedLoopStart); FIELD(lfoCycles); FIELD(lfoStep); FIELD(alfoOutput); FIELD(sample1); FIELD(sample2);
        FIELD(output); FIELD(finalLevel);
#undef FIELD
    }
    const DSP &a = ref.m_dsp;
    const Dsp &b = chip.m_dsp;
#define MEMORY(F) SAME("DSP " #F, 0, memcmp(a.F.data(), b.F.data(), sizeof(a.F)))
    MEMORY(program); MEMORY(tempMem); MEMORY(soundMem); MEMORY(coeffs); MEMORY(addrs); MEMORY(mixStack); MEMORY(effectOut);
    MEMORY(audioInOut);
#undef MEMORY
#define FIELD(F) SAME("DSP " #F, a.F, b.F)
    FIELD(PC); FIELD(m_RBP); FIELD(m_RBL); FIELD(m_programLength); FIELD(INPUTS); FIELD(SFT_REG); FIELD(FRC_REG); FIELD(Y_REG);
    FIELD(ADRS_REG); FIELD(MDEC_CT); FIELD(m_mixStackGen); FIELD(m_mixStackNull); FIELD(m_readPending); FIELD(m_readNOFL);
    FIELD(m_readValue); FIELD(m_writePending); FIELD(m_writeValue); FIELD(m_readWriteAddr);
#undef FIELD
    SAME("stack", 0, memcmp(ref.m_soundStack.data(), chip.m_soundStack.data(), sizeof(ref.m_soundStack)));
    SAME("stack index", ref.m_soundStackIndex, chip.m_soundStackIndex);
    SAME("noise", ref.m_lfsr, chip.m_lfsr);
    SAME("KYONEX taken", ref.m_kyonexExecute, chip.m_kyonexExecute);
    SAME("CD audio position", ref.m_cddaReadPos, chip.m_cddaReadPos);
    SAME("CD audio ready", ref.m_cddaReady, chip.m_cddaReady);
    SAME("out left", ref.m_out[0], chip.m_out[0]);
    SAME("out right", ref.m_out[1], chip.m_out[1]);
#undef SAME
    m_quiet = false;
    m_steps++;
    if (different) {
        m_stepsDifferent++;
        if (m_reportsLeft) {
            m_reportsLeft--;
            fprintf(stderr, ": sound: step %u of sample %llu differs (reference/here)\n", i, static_cast<unsigned long long>(m_steps / 32));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// The sound CPU's interpreter against Ymir's

struct Generator::CpuCheck : M68KBus {
    Generator &owner;
    ymir::m68k::MC68EC000 *cpu = nullptr;
    M68K mine{*this};
    struct Write {
        uint32_t address;
        uint32_t value;
        bool word;
    };
    std::vector<Write> writes; // what the instruction wrote (not done: Ymir's CPU does it)
    uint64_t instructions = 0, different = 0;
    unsigned reportsLeft = 40;
    std::set<std::string> seen;

    explicit CpuCheck(Generator &owner)
        : owner(owner) {}

    // (a read sees what the instruction itself wrote before)
    bool Written(uint32_t address, uint8_t &value) const {
        bool found = false;
        for (const Write &w : writes) {
            if (w.word && (w.address | 1u) == (address | 1u)) {
                value = static_cast<uint8_t>((address & 1) ? w.value : w.value >> 8);
                found = true;
            } else if (!w.word && w.address == address) {
                value = static_cast<uint8_t>(w.value);
                found = true;
            }
        }
        return found;
    }
    uint8_t Read8(uint32_t address) override {
        uint8_t value;
        return Written(address, value) ? value : owner.CpuRead8(address);
    }
    uint16_t Read16(uint32_t address) override {
        uint16_t value = owner.CpuRead16(address);
        uint8_t byte;
        if (Written(address, byte)) {
            value = static_cast<uint16_t>((value & 0x00FF) | (byte << 8));
        }
        if (Written(address | 1u, byte)) {
            value = static_cast<uint16_t>((value & 0xFF00) | byte);
        }
        return value;
    }
    void Write8(uint32_t address, uint8_t value) override {
        writes.push_back({address, value, false});
    }
    void Write16(uint32_t address, uint16_t value) override {
        writes.push_back({address, value, true});
    }

    uint64_t Run(ymir::m68k::MC68EC000 &c) {
        cpu = &c;
        for (unsigned i = 0; i < 8; i++) {
            mine.d[i] = M68KAccess::D(c, i);
            mine.a[i] = M68KAccess::A(c, i);
        }
        const uint16_t sr = M68KAccess::SR(c);
        mine.supervisor = sr & 0x2000;
        mine.SetSR(sr);
        mine.otherSP = M68KAccess::OtherSP(c);
        mine.pc = M68KAccess::PC(c) - 4;
        mine.stopped = false;
        mine.SetInterruptLevel(M68KAccess::InterruptLevel(c));
        const uint32_t at = mine.pc;
        const uint16_t op = owner.CpuRead16(at);
        writes.clear();
        const uint64_t myCycles = mine.Step();

        owner.m_writeLog.clear();
        owner.m_logWrites = true;
        const uint64_t cycles = c.RecompInterpretNext();
        owner.m_logWrites = false;

        instructions++;
        char what[256] = "";
        auto differs = [&](const char *name, uint64_t theirs, uint64_t here) {
            if (theirs != here && !what[0]) {
                snprintf(what, sizeof(what), "%s %llX/%llX", name, static_cast<unsigned long long>(theirs), static_cast<unsigned long long>(here));
            }
        };
        static const char *const kNames[16] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};
        for (unsigned i = 0; i < 8; i++) {
            differs(kNames[i], M68KAccess::D(c, i), mine.d[i]);
            differs(kNames[8 + i], M68KAccess::A(c, i), mine.a[i]);
        }
        differs("other SP", M68KAccess::OtherSP(c), mine.otherSP);
        differs("PC", M68KAccess::PC(c) - 4, mine.pc);
        differs("SR", M68KAccess::SR(c), mine.SR());
        differs("cycles", cycles, myCycles);
        // What was written: sound RAM must hold it now; the chip's registers must have been written the same
        size_t registerWrites = 0;
        for (const Write &w : writes) {
            const uint32_t address = w.address & 0xFFFFFF;
            if (address < 0x80000) {
                differs("memory", w.word ? owner.CpuRead16(address) : owner.CpuRead8(address), w.value);
            } else if (address >= 0x100000 && address < 0x200000) {
                // (the same writes, in whichever order: the two words of a long word go either way round)
                bool found = false;
                for (RegisterWrite &theirs : owner.m_writeLog) {
                    if (theirs.address == (address & 0xFFF) && theirs.value == w.value && theirs.word == w.word) {
                        theirs.address = 0xFFFFFFFF;
                        found = true;
                        break;
                    }
                }
                differs("register write", 1, found);
                registerWrites++;
            }
        }
        differs("register writes", owner.m_writeLog.size(), registerWrites);
        if (what[0]) {
            different++;
            // (one report for each kind of instruction and difference)
            const std::string kind = std::to_string(op & 0xF1F8) + std::string(what, strcspn(what, " "));
            if (reportsLeft && seen.insert(kind).second) {
                reportsLeft--;
                fprintf(stderr, "sound CPU: instruction %04X at %06X: %s (reference/here)\n", op, at, what);
            }
        }
        return cycles;
    }
};

// (memory and registers as the sound CPU sees them)
uint8 Generator::CpuRead8(uint32 address) {
    return m_scsp->Read<uint8, false>(address & 0xFFFFFF);
}
uint16 Generator::CpuRead16(uint32 address) {
    return m_scsp->Read<uint16, false>(address & 0xFFFFFE);
}

void Generator::InstallCpuCheck() {
    m_cpuCheck = std::make_shared<CpuCheck>(*this);
    m_scsp->GetM68K().SetRecompDispatcher(
        [](ymir::m68k::MC68EC000 &cpu, uint64 budget, void *context) -> uint64 {
            (void)budget;
            return static_cast<CpuCheck *>(context)->Run(cpu);
        },
        m_cpuCheck.get());
}

void Generator::Report() const {
    if (m_cpuCheck) {
        fprintf(stderr, "sound CPU: %llu instructions; %llu left something different from the reference\n",
                static_cast<unsigned long long>(m_cpuCheck->instructions), static_cast<unsigned long long>(m_cpuCheck->different));
    }
    if (m_check) {
        fprintf(stderr, "sound: %llu samples; steps where the chip's state differs from the reference's: %llu of %llu\n",
                static_cast<unsigned long long>(m_steps / 32), static_cast<unsigned long long>(m_stepsDifferent),
                static_cast<unsigned long long>(m_steps));
    }
}

} // namespace satrecomp::sound
