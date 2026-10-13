// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Save states: the sound chip's and its CPU's part (runtime/state.h).
#include "chip.h"
#include "m68k.h"

#include "../runtime/state.h"

namespace satrecomp::sound {

void M68K::Serialize(Archive &ar) {
    ar.Mark(0x4D36384B);
    ar.Pod(d);
    ar.Pod(a);
    ar.Pod(otherSP);
    ar.Pod(pc);
    ar.Pod(flagC);
    ar.Pod(flagV);
    ar.Pod(flagZ);
    ar.Pod(flagN);
    ar.Pod(flagX);
    ar.Pod(supervisor);
    ar.Pod(trace);
    ar.Pod(interruptMask);
    ar.Pod(stopped);
    ar.Pod(m_interruptLevel);
}

void Chip::Serialize(Archive &ar) {
    ar.Mark(0x53435350);
    ar.Pod(m_slots);
    uint8 *ram = m_dsp.m_WRAM;
    ar.Pod(m_dsp);
    m_dsp.m_WRAM = ram; // (where sound RAM is does not come from the state)
    ar.Pod(m_soundStack);
    ar.Pod(m_soundStackIndex);
    ar.Pod(m_kyonex);
    ar.Pod(m_kyonexExecute);
    ar.Pod(m_masterVolume);
    ar.Pod(m_dac18Bits);
    ar.Pod(m_mem4MB);
    ar.Pod(m_monitorSlotCall);
    ar.Pod(m_timers);
    ar.Pod(m_scuEnabledInterrupts);
    ar.Pod(m_scuPendingInterrupts);
    ar.Pod(m_m68kEnabledInterrupts);
    ar.Pod(m_m68kPendingInterrupts);
    ar.Pod(m_m68kInterruptLevels);
    ar.Pod(m_m68kLevel);
    ar.Pod(m_dmaExec);
    ar.Pod(m_dmaXferToMem);
    ar.Pod(m_dmaGate);
    ar.Pod(m_dmaMemAddress);
    ar.Pod(m_dmaRegAddress);
    ar.Pod(m_dmaXferLength);
    ar.Pod(m_sampleCounter);
    ar.Pod(m_lfsr);
    ar.Pod(m_out);
    ar.Pod(m_lastOut);
    ar.Pod(m_cddaBuffer);
    ar.Pod(m_cddaReadPos);
    ar.Pod(m_cddaWritePos);
    ar.Pod(m_cddaReady);
}

} // namespace satrecomp::sound
