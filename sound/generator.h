// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The sound chip (chip.h) connected to the Ymir hardware model. Ymir's sound chip still has the registers the
// two CPUs talk to, its timers and interrupts, and runs the sound CPU; every register write, reset and sector
// of CD audio is passed on to satrecomp's chip, which makes the samples.
//
// With `check`, Ymir's own sample generation runs as well and after each of a sample's 32 steps the two
// chips' state is compared (voices, DSP, timers, interrupts); reported at exit.
//
// This file and its .cpp are the only sound code that knows Ymir.
#pragma once

#include <ymir/hw/scsp/scsp.hpp>

#include "chip.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace satrecomp::sound {

class Generator {
public:
    // Takes over the chip's sample generation
    void Install(ymir::scsp::SCSP &scsp, bool check);
    void Report() const;
    // CD audio from somewhere other than the chip's own buffer (which the CD drive fills)
    void SetCdAudioSource(bool (*source)(int16_t &left, int16_t &right)) {
        m_chip->cdAudioSource = source;
    }
    // SATRECOMP_M68K=check: every instruction of the sound CPU is run by satrecomp's interpreter (m68k.h)
    // as well, from the same state, and what the two leave is compared; reported at exit
    void InstallCpuCheck();
    Chip &Core() {
        return *m_chip;
    }

private:
    ymir::scsp::SCSP *m_scsp = nullptr;
    std::unique_ptr<Chip> m_chip;
    bool m_check = false;
    uint64_t m_steps = 0, m_stepsDifferent = 0;
    unsigned m_reportsLeft = 12;

    void CheckedStep(uint32 i);
    bool Differs(const char *what, long long theirs, long long mine);
    bool m_quiet = false;

    struct RegisterWrite {
        uint32 address;
        uint16 value;
        bool word;
    };
    struct CpuCheck;
    uint8 CpuRead8(uint32 address);
    uint16 CpuRead16(uint32 address);
    std::shared_ptr<CpuCheck> m_cpuCheck;
    bool m_logWrites = false;
    std::vector<RegisterWrite> m_writeLog;
};

} // namespace satrecomp::sound
