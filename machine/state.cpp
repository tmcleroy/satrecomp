// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Save states: the machine's part (runtime/state.h).
#include "machine.h"

#include "../runtime/state.h"

namespace satrecomp::machine {

void SH2::Serialize(Archive &ar) {
    ar.Mark(0x53483220);
    ar.Pod(R);
    ar.Pod(PC);
    ar.Pod(PR);
    ar.Pod(GBR);
    ar.Pod(VBR);
    ar.Pod(MAC.u64);
    ar.Pod(SR.u32);
    ar.Pod(m_intrFlags);
    ar.Pod(m_delaySlot);
    ar.Pod(m_delaySlotTarget);
    ar.Pod(m_wbReg);
    ar.Pod(m_recompResume);
    ar.Pod(m_recompYield);
    ar.Pod(m_recompWaiting);
    ar.Pod(m_sleep);
    ar.Pod(m_yieldRequest);
    ar.Pod(INTC);
    ar.Pod(m_frt);
    ar.Pod(m_divu);
    ar.Pod(m_dma);
    ar.Pod(m_dmaOperation);
    ar.Pod(m_cacheControl);
    ar.Pod(m_cacheData);
}

void Machine::Serialize(Archive &ar) {
    ar.Mark(0x4D414348);
    ar.Vec(m_ramHigh);
    ar.Vec(m_ramLow);
    ar.Vec(m_soundRam);
    ar.Vec(m_vram1);
    ar.Vec(m_fb[0]);
    ar.Vec(m_fb[1]);
    ar.Vec(m_vram2);
    ar.Vec(m_cram);
    ar.Pod(m_now);
    ar.Pod(m_masterSpillover);
    ar.Pod(m_slaveSpillover);
    ar.Pod(m_fineCycles);
    ar.Pod(m_clock);
    ar.Pod(m_clock352);
    ar.Pod(slaveEnabled);
    ar.Pod(m_scu);
    ar.Pod(m_smpc);
    ar.Pod(m_vdp);
    ar.Pod(m_cd);
    ar.Pod(m_soundCpuEnabled);
    ar.Pod(m_soundCpuSpillover);
    ar.Pod(m_sampleDue);
    ar.Pod(m_sampleFraction);
    masterSH2.Serialize(ar);
    slaveSH2.Serialize(ar);
    m_soundCpu.Serialize(ar);
    m_sound->Serialize(ar);
    m_video->Serialize(ar);
    ar.Mark(0x454E444D);
    if (!ar.Saving()) {
        // (the RAM vectors keep their place, but in case: what points into them)
        ramHigh = m_ramHigh.data();
        ramLow = m_ramLow.data();
        ++g_codeEpoch; // (work RAM is other RAM now: translations are compared with it again)
        ++g_activity;
    }
}

} // namespace satrecomp::machine
