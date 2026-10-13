// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "chip.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace satrecomp::sound {

namespace {
    uint16 Be16(const uint8 *p) {
        return static_cast<uint16>((p[0] << 8) | p[1]);
    }
    void PutBe16(uint8 *p, uint16 value) {
        p[0] = static_cast<uint8>(value >> 8);
        p[1] = static_cast<uint8>(value);
    }
    // The low `bits` bits of a value as a signed number
    constexpr sint32 Signed(uint32 value, unsigned bits) {
        const uint32 sign = 1u << (bits - 1);
        value &= (sign << 1) - 1u;
        return static_cast<sint32>((value ^ sign) - sign);
    }
    template <unsigned N>
    constexpr sint32 SignedN(uint32 value) {
        return Signed(value, N);
    }
    using Loop = Slot::LoopControl;
} // namespace

namespace {
    // The envelope generator moves at one of 64 rates. A rate has a period (it acts every 2^shift samples)
    // and a pattern of eight amounts it steps by, in turn.
    struct Rate {
        uint32 shift, mask;
        uint8 amount[8];
    };
    const std::array<Rate, 64> kRates = [] {
        static constexpr uint8 kPatterns[4][8] = {
            {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}};
        static constexpr uint8 kFast[4][8] = {
            {1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {2, 1, 2, 1, 2, 1, 2, 1}, {1, 2, 2, 2, 1, 2, 2, 2}};
        std::array<Rate, 64> rates{};
        for (uint32 rate = 0; rate < 64; rate++) {
            Rate &r = rates[rate];
            r.shift = rate >= 44 ? 1 : 12 - (rate >> 2);
            r.mask = (1u << r.shift) - 1;
            for (uint32 i = 0; i < 8; i++) {
                if (rate < 2) {
                    r.amount[i] = 0;
                } else if (rate < 48) {
                    r.amount[i] = kPatterns[rate & 3][i];
                } else if (rate < 60) {
                    r.amount[i] = static_cast<uint8>(kFast[rate & 3][i] << ((rate - 48) >> 2));
                } else {
                    r.amount[i] = 8;
                }
            }
        }
        return rates;
    }();

    // Key rate scaling: higher octaves make the envelope faster
    sint32 RateScaling(const Slot &slot) {
        return std::clamp<sint32>(slot.keyRateScaling + static_cast<sint32>(slot.octave ^ 8) - 8, 0, 15);
    }

    // LFO waveforms by step (0..255): for pitch, signed around zero; for amplitude, from zero up
    sint32 PitchWave(Slot::Waveform wave, uint8 step, uint32 noise) {
        switch (wave) {
        case Slot::Waveform::Saw: return static_cast<sint8>(step) & ~1;
        case Slot::Waveform::Square: return step < 128 ? 126 : -128;
        case Slot::Waveform::Triangle: {
            const uint8 at = static_cast<uint8>(step + 64); // (rises for half the cycle, falls for the other)
            return (at < 128 ? at : 255 - at) * 2 - 128;
        }
        default: return static_cast<sint8>((noise ^ 0x80) & ~1u);
        }
    }
    uint32 AmplitudeWave(Slot::Waveform wave, uint8 step, uint32 noise) {
        switch (wave) {
        case Slot::Waveform::Saw: return step & ~1u;
        case Slot::Waveform::Square: return step < 128 ? 0x00 : 0xFE;
        case Slot::Waveform::Triangle: return (step < 128 ? step : 255 - step) * 2u;
        default: return static_cast<uint8>(noise) & ~1u;
        }
    }

    // The DSP's wave memory holds 16-bit floating point numbers: sign, 4-bit exponent, 11-bit mantissa
    uint32 FromFloat(uint16 value) {
        const uint32 sign = static_cast<sint32>((value & 0x8000) << 16) >> 1;
        const uint32 exponent = (value >> 11) & 0xF;
        uint32 result = value & 0x7FF;
        if (exponent < 12) {
            result |= 0x800;
        }
        result <<= 19;
        result ^= sign;
        result = static_cast<sint32>(result) >> (8 + std::min<uint32>(11, exponent));
        return result & 0xFFFFFF;
    }
    uint32 ToFloat(uint32 value) {
        const uint32 shifted = value << 8;
        const uint32 sign = static_cast<sint32>(shifted) >> 31;
        const uint32 exponent = std::min(0x1F, std::countl_zero(((shifted ^ sign) << 1) | (1u << 19)));
        const uint32 shift = exponent - (exponent == 12);
        uint32 result = static_cast<sint32>(shifted) >> (19 - shift);
        result &= 0x87FF;
        result |= exponent << 11;
        return result;
    }
} // namespace

// Key on starts a voice that is in release (or has never played); key off puts one that is playing into
// release. Happens for all voices at once when the game writes "execute".
bool Chip::Trigger(Slot &slot) {
    const bool releasing = slot.egState == Slot::EGState::Release;
    if (releasing != slot.keyOnBit) {
        return false;
    }
    if (!slot.keyOnBit) {
        slot.egState = Slot::EGState::Release;
        slot.currEGRate = slot.releaseRate;
        return true;
    }
    slot.active = true;
    slot.egState = Slot::EGState::Attack;
    slot.currEGRate = slot.attackRate;
    // (an attack too fast for the chip never starts: the envelope stays at full level. As the hardware model
    // has it, a scaling below zero counts as the highest here, unlike for the rates themselves.)
    const uint8 scaling = std::min<uint8>(static_cast<uint8>(slot.keyRateScaling + static_cast<sint8>(slot.octave ^ 8) - 8), 0xF);
    slot.egAttackBug = slot.keyRateScaling != 0xF && slot.attackRate + scaling >= 0x20;
    slot.egLevel = slot.keyRateScaling != 0xF && slot.egAttackBug ? 0x000 : 0x280;
    slot.currEGLevel = slot.egLevel;
    slot.currSample = 0;
    slot.currPhase = 0;
    slot.nextPhase = 0;
    slot.modulation = 0;
    slot.reverse = false;
    slot.crossedLoopStart = false;
    slot.sample1 = 0;
    slot.sample2 = 0;
    slot.output = 0;
    return true;
}

void Chip::Pitch(Slot &slot) {
    Chip &chip = *this;
    chip.m_lfsr = (chip.m_lfsr >> 1) | (((chip.m_lfsr >> 5) ^ chip.m_lfsr) & 1u) << 16; // the noise generator
    if (slot.index == 0) {
        chip.m_kyonexExecute = chip.m_kyonex;
        chip.m_kyonex = false;
    }
    if (chip.m_kyonexExecute) {
        Trigger(slot);
    }

    // Vibrato: the LFO's wave, by its sensitivity, as a fraction of the pitch
    sint32 vibrato = 0;
    if (slot.pitchLFOSens != 0) {
        vibrato = PitchWave(slot.pitchLFOWaveform, slot.lfoStep, chip.m_lfsr);
        vibrato >>= 7 - static_cast<sint32>(slot.pitchLFOSens);
        vibrato *= slot.freqNumSwitch >> 4;
        vibrato >>= 6;
    }
    if (++slot.lfoCycles >= slot.lfoStepInterval) {
        slot.lfoCycles = 0;
        ++slot.lfoStep;
    }
    if (slot.lfoReset) {
        slot.lfoStep = 0;
    }

    // The phase: how far through the wave, in 1/16384ths of a sample, the voice moves this sample
    if (!slot.active) {
        slot.currPhase = 0;
        return;
    }
    slot.currPhase = slot.nextPhase;
    const uint32 step = ((slot.freqNumSwitch + vibrato) << (slot.octave ^ 8u)) >> 4;
    slot.nextPhase = (slot.nextPhase & 0x3FFF) + step;
}

void Chip::Advance(Slot &slot) {
    if (slot.soundSource == Slot::SoundSource::SoundRAM && !slot.active) {
        return;
    }
    slot.currSample += slot.currPhase >> 14;
    if (!slot.crossedLoopStart) {
        const uint16 next = static_cast<uint16>(slot.currSample + 1);
        if (next > slot.loopStartAddress) {
            slot.crossedLoopStart = true;
            if (slot.loopControl == Loop::Reverse) {
                // (a reverse loop plays from its end backwards once the start has been reached)
                slot.currSample -= slot.loopStartAddress + slot.loopEndAddress;
                slot.reverse = true;
            }
        }
        return;
    }
    const uint16 next = static_cast<uint16>((slot.reverse ? ~slot.currSample : slot.currSample) + 1);
    const bool backwardsLoop = slot.reverse && (slot.loopControl == Loop::Reverse || slot.loopControl == Loop::Alternate);
    const bool crossed = next > (backwardsLoop ? slot.loopStartAddress : slot.loopEndAddress);
    if (slot.reverse == crossed) {
        return;
    }
    switch (slot.loopControl) {
    case Loop::Off: // the end of the wave: the voice stops
        slot.active = false;
        slot.reverse = false;
        slot.crossedLoopStart = false;
        break;
    case Loop::Normal:
        if (slot.reverse) {
            slot.currSample += slot.loopEndAddress - slot.loopStartAddress;
        } else {
            slot.currSample += slot.loopStartAddress - slot.loopEndAddress;
        }
        break;
    case Loop::Reverse: slot.currSample += slot.loopStartAddress - slot.loopEndAddress; break;
    case Loop::Alternate:
        slot.reverse = !slot.reverse;
        if (slot.reverse) {
            slot.currSample -= slot.loopEndAddress * 2u;
        } else {
            slot.currSample += slot.loopStartAddress * 2u;
        }
        break;
    }
}

void Chip::Fetch(Slot &slot) {
    Chip &chip = *this;
    if (slot.soundSource == Slot::SoundSource::SoundRAM && !slot.active) {
        return;
    }
    switch (slot.soundSource) {
    case Slot::SoundSource::SoundRAM: {
        // (the sample after the position; the one at the position was read two steps earlier, in Step)
        const Slot &after = chip.m_slots[(slot.index + 1) & 31];
        const uint16 position = static_cast<uint16>((slot.reverse ? ~slot.currSample : slot.currSample) + 1);
        const sint32 phase = after.reverse ? ~after.currPhase : after.currPhase;
        const sint32 fraction = ((phase >> 8) & 0x3F) + ((slot.modulation & 0x1F) << 1);
        const sint32 offset = SignedN<17>((position + SignedN<11>(slot.modulation >> 5) + (fraction >> 6)) & slot.mask);
        if (slot.pcm8Bit) {
            slot.sample2 = static_cast<sint16>(static_cast<sint8>(chip.m_WRAM[(slot.startAddress + offset) & 0x7FFFF]) << 8);
        } else {
            slot.sample2 = static_cast<sint16>(Be16(&chip.m_WRAM[((slot.startAddress & ~1u) + offset * 2) & 0x7FFFF]));
        }
        break;
    }
    case Slot::SoundSource::Noise: slot.sample1 = static_cast<sint16>((chip.m_lfsr & 0xFF) << 8); break;
    default: slot.sample1 = 0; break;
    }
    slot.sample1 ^= slot.sampleXOR;
    slot.sample2 ^= slot.sampleXOR;
}

void Chip::Envelope(Slot &slot) {
    Chip &chip = *this;
    if (slot.soundSource == Slot::SoundSource::SoundRAM && chip.linearInterpolation) {
        // Between the two samples, by the fraction of the position
        const sint32 phase = slot.reverse ? ~slot.currPhase : slot.currPhase;
        const sint32 fraction = (((phase >> 8) & 0x3F) + ((slot.modulation & 0x1F) << 1)) & 0x3F;
        slot.output = static_cast<sint16>(slot.sample1 + (((slot.sample2 - slot.sample1) * fraction) >> 6));
    } else {
        slot.output = slot.sample1;
    }

    // Tremolo
    slot.alfoOutput = 0;
    if (slot.ampLFOSens != 0) {
        slot.alfoOutput = static_cast<uint8>(AmplitudeWave(slot.ampLFOWaveform, slot.lfoStep, chip.m_lfsr) >> (7u - slot.ampLFOSens));
    }

    // The envelope generator: attack (level falls to 0, that is full volume, ever slower), decay 1 down to
    // the decay level, decay 2, and release after key off; levels are attenuations, 0x3FF silent
    const uint32 counter = static_cast<uint32>(chip.m_sampleCounter + 1);
    const uint8 currentRate = slot.currEGRate;
    uint32 effective = currentRate;
    if (slot.keyRateScaling < 0xF) {
        effective += RateScaling(slot);
    }
    const Rate &rate = kRates[std::min<uint32>(effective << 1, 0x3F)];
    const uint32 amount = (counter & rate.mask) != 0 ? 0 : rate.amount[(counter >> rate.shift) & 7];
    const uint32 shownBefore = slot.currEGLevel;
    const uint32 level = slot.egLevel;
    slot.currEGLevel = slot.egBypass || (slot.egState == Slot::EGState::Attack && slot.egHold) ? 0x000 : slot.egLevel;
    switch (slot.egState) {
    case Slot::EGState::Attack:
        if (!slot.egAttackBug && amount > 0 && slot.egLevel > 0 && currentRate > 0) {
            slot.egLevel += static_cast<sint32>(~level * amount) >> 4;
        }
        if (slot.loopStartLink ? slot.crossedLoopStart : level == 0) {
            slot.egState = Slot::EGState::Decay1;
            slot.currEGRate = slot.decay1Rate;
        }
        break;
    case Slot::EGState::Decay1:
        if ((slot.egLevel >> 5) == slot.decayLevel) {
            slot.egState = Slot::EGState::Decay2;
            slot.currEGRate = slot.decay2Rate;
        }
        [[fallthrough]];
    default:
        if (currentRate > 0) {
            slot.egLevel = static_cast<uint16>(std::min<uint32>(slot.egLevel + amount, 0x3FF));
        }
        break;
    }
    if (shownBefore >= 0x3C0 && !slot.egBypass) { // faded out: the voice stops
        slot.active = false;
        slot.reverse = false;
        slot.crossedLoopStart = false;
    }
}

void Chip::Level(Slot &slot) {
    if (slot.soundSource == Slot::SoundSource::SoundRAM && !slot.active) {
        return;
    }
    slot.finalLevel = std::min<sint32>(slot.alfoOutput + slot.currEGLevel + (slot.totalLevel << 2), 0x3FF);
}

void Chip::Attenuate(Slot &slot) {
    if (slot.soundSource == Slot::SoundSource::SoundRAM && !slot.active) {
        slot.output = slot.currEGLevel < 0x3C0 ? slot.sampleXOR : 0;
    } else if (!slot.soundDirect) {
        // (the level is in 1/64ths of 6 dB: a shift and a multiplication)
        slot.output = static_cast<sint16>((slot.output * ((slot.finalLevel & 0x3F) ^ 0x7F)) >> ((slot.finalLevel >> 6) + 7));
    }
}

// One instruction of the DSP's program (128 a sample): a multiply-accumulate with inputs from the mixed
// voices, its own memories and a ring buffer in wave memory (for delays: reverb, chorus), results to the
// effect outputs.
void Chip::Effects() {
    Dsp &dsp = m_dsp;
    auto flush = [&] {
        const uint32 address = dsp.m_readWriteAddr * 2;
        if (address < 0x80000) {
            PutBe16(&dsp.m_WRAM[address], dsp.m_writeValue);
        }
        dsp.m_writePending = false;
    };
    if (dsp.PC < dsp.m_programLength) {
        const DspInstr op = dsp.program[dsp.PC];
        if (op.IRA <= 0x1F) {
            dsp.INPUTS = dsp.soundMem[op.IRA];
        } else if (op.IRA <= 0x2F) {
            dsp.INPUTS = dsp.mixStack[((op.IRA & 0xF) | dsp.m_mixStackGen) ^ 0x10] << 4;
        } else if (op.IRA <= 0x31) {
            dsp.INPUTS = dsp.audioInOut[op.IRA & 1] << 8;
        }
        const sint32 inputs = dsp.INPUTS;
        const sint32 temp = dsp.tempMem[(op.TRA + dsp.MDEC_CT) & 0x7F];
        const sint32 x = op.XSEL ? inputs : temp;
        uint16 y = 0;
        switch (op.YSEL) {
        case 0: y = dsp.FRC_REG; break;
        case 1: y = dsp.coeffs[op.CRA]; break;
        case 2: y = static_cast<uint16>((dsp.Y_REG >> 11) & 0x1FFF); break;
        default: y = static_cast<uint16>((dsp.Y_REG >> 4) & 0xFFF); break;
        }
        if (op.YRL) {
            dsp.Y_REG = inputs & 0xFFFFFF;
        }
        // The accumulator, shifted and either clamped or wrapped to 24 bits
        sint32 shifted = static_cast<sint32>(static_cast<uint32>(SignedN<26>(dsp.SFT_REG)) << (op.SHFT0 ^ op.SHFT1));
        shifted = op.SHFT1 == 0 ? std::clamp(shifted, -0x800000, 0x7FFFFF) : SignedN<24>(shifted);
        if (op.FRCL) {
            dsp.FRC_REG = static_cast<uint16>(op.SHFT == 3 ? (shifted & 0xFFF) : ((shifted >> 11) & 0x1FFF));
        }
        uint32 addend = 0;
        if (!op.ZERO) {
            addend = op.BSEL ? dsp.SFT_REG : static_cast<uint32>(temp);
            if (op.NEGB) {
                addend = 0u - addend;
            }
        }
        const uint32 product = static_cast<uint32>((static_cast<sint64>(SignedN<13>(y)) * x) >> 12);
        dsp.SFT_REG = (product + addend) & 0x3FFFFFF;
        if (op.EWT) {
            dsp.effectOut[op.EWA] = static_cast<sint16>(shifted >> 8);
        }
        if (op.TWT) {
            dsp.tempMem[(op.TWA + dsp.MDEC_CT) & 0x7F] = shifted;
        }
        if (op.IWT) {
            dsp.soundMem[op.IWA] = SignedN<24>(dsp.m_readValue);
        }
        // Wave memory is read or written an instruction after it was asked for
        if (dsp.m_readPending) {
            const uint32 address = dsp.m_readWriteAddr * 2;
            const uint16 value = address < 0x80000 ? Be16(&dsp.m_WRAM[address]) : 0;
            dsp.m_readValue = dsp.m_readNOFL ? static_cast<uint32>(value << 8) : FromFloat(value);
            dsp.m_readPending = false;
            dsp.m_readNOFL = false;
        } else if (dsp.m_writePending) {
            flush();
        }
        uint16 address = static_cast<uint16>(dsp.addrs[op.MASA] + op.NXADR);
        if (op.ADREB) {
            address = static_cast<uint16>(address + SignedN<12>(dsp.ADRS_REG));
        }
        if (!op.TABLE) {
            address = static_cast<uint16>((address + dsp.MDEC_CT) & dsp.m_RBL);
        }
        dsp.m_readWriteAddr = (address + dsp.m_RBP) & 0x7FFFF;
        if (op.MRD) {
            dsp.m_readPending = true;
            dsp.m_readNOFL = op.NOFL;
        }
        if (op.MWT) {
            dsp.m_writePending = true;
            dsp.m_writeValue = static_cast<uint16>(op.NOFL ? (shifted >> 8) : ToFloat(shifted));
        }
        if (op.ADRL) {
            dsp.ADRS_REG = static_cast<uint16>(op.SHFT == 3 ? ((shifted >> 12) & 0xFFF) : ((inputs >> 16) & 0xFFF));
        }
    } else if (dsp.m_writePending) {
        flush();
    }
    if (++dsp.PC == 0x80) {
        dsp.PC = 0;
        dsp.m_mixStackGen ^= 0x10; // (the voices mix into one set of inputs while the program reads the other)
        dsp.m_mixStackNull = 0xFFFF;
        --dsp.MDEC_CT;
    }
}

// A voice or effect output into the left and right sums: a send level (6 dB a step) and a pan that turns one
// side down (3 dB a step)
void Chip::Mix(sint32 output, uint8 sendLevel, uint8 pan) {
    if (sendLevel == 0) {
        return;
    }
    output <<= 14;
    output >>= sendLevel ^ 7u;
    const uint32 amount = pan & 0xF;
    sint32 turnedDown = 0;
    if (amount != 0xF) {
        turnedDown = output >> (amount >> 1);
        if (amount & 1) {
            turnedDown -= turnedDown >> 2;
        }
    }
    const bool right = pan & 0x10;
    m_out[0] += (right ? output : turnedDown) >> 14;
    m_out[1] += (right ? turnedDown : output) >> 14;
}

void Chip::Finish() {
    Chip &chip = *this;
    for (sint32 &out : chip.m_out) {
        // Master volume, 3 dB a step
        if (chip.m_masterVolume == 0) {
            out = 0;
        } else {
            const uint32 volume = chip.m_masterVolume ^ 0xF;
            out <<= 8;
            out >>= volume >> 1;
            if (volume & 1) {
                out -= out >> 2;
            }
            out >>= 8;
        }
        out = std::clamp<sint32>(out, -32768, 32767);
        if (chip.m_dac18Bits) {
            out = static_cast<sint32>(static_cast<uint32>(out) << 2);
        }
    }
    m_lastOut = m_out;
    if (emit && onSample) {
        onSample(static_cast<sint16>(chip.m_out[0]), static_cast<sint16>(chip.m_out[1]));
    }
    chip.m_out.fill(0);

    // The next sample of CD audio, which the DSP and voices 16 and 17 can use
    int16_t left, right;
    if (emit && cdAudioSource && cdAudioSource(left, right)) {
        chip.m_dsp.audioInOut[0] = left;
        chip.m_dsp.audioInOut[1] = right;
    } else if (chip.m_cddaReady && chip.m_cddaReadPos != chip.m_cddaWritePos) {
        chip.m_dsp.audioInOut[0] = static_cast<sint16>((chip.m_cddaBuffer[chip.m_cddaReadPos] | (chip.m_cddaBuffer[(chip.m_cddaReadPos) + 1] << 8)));
        chip.m_dsp.audioInOut[1] = static_cast<sint16>((chip.m_cddaBuffer[chip.m_cddaReadPos + 2] | (chip.m_cddaBuffer[(chip.m_cddaReadPos + 2) + 1] << 8)));
        chip.m_cddaReadPos = (chip.m_cddaReadPos + 4) % chip.m_cddaBuffer.size();
    } else {
        chip.m_dsp.audioInOut[0] = 0;
        chip.m_dsp.audioInOut[1] = 0;
        chip.m_cddaReady = false;
    }
}

// Step i of a sample: voice i enters the pipeline, voices i-1 .. i-6 move a stage on
void Chip::Step(uint32 i) {
    Chip &chip = *this;
    Dsp &dsp = chip.m_dsp;
    auto &slots = chip.m_slots;
    Slot &s1 = slots[i], &s2 = slots[(i - 1) & 31], &s3 = slots[(i - 2) & 31], &s4 = slots[(i - 3) & 31], &s5 = slots[(i - 4) & 31],
         &s6 = slots[(i - 5) & 31], &s7 = slots[(i - 6) & 31];

    // The finished voice's output goes on the sound stack (other voices can use it to modulate their own)
    if (!s7.stackWriteInhibit) {
        chip.m_soundStack[(chip.m_soundStackIndex - 6u) & 63] = static_cast<uint16>(s7.output);
    }
    Effects();

    // ... and into one of the DSP's 16 inputs
    const uint32 input = s7.inputSelect;
    const bool first = dsp.m_mixStackNull & (1u << input);
    sint32 &mixed = dsp.mixStack[input | dsp.m_mixStackGen];
    if (s7.inputMixingLevel > 0) {
        const sint32 value = SignedN<20>((s7.output << 4) >> (s7.inputMixingLevel ^ 7));
        mixed = first ? value : mixed + value;
        dsp.m_mixStackNull &= ~(1u << input);
    } else if (first) {
        mixed = 0;
        dsp.m_mixStackNull &= ~(1u << input);
    }

    // Modulation inputs from the sound stack, and the sample at the wave position
    if (s2.modLevel >= 5) {
        s2.modXSample = static_cast<sint16>(chip.m_soundStack[(chip.m_soundStackIndex - 1 + s2.modXSelect) & 63]);
    }
    if (s3.soundSource == Slot::SoundSource::SoundRAM && s3.active) {
        if (s3.modLevel >= 5) {
            const sint32 sum = (s3.modXSample + s3.modYSample) & 0x3FFFFE;
            s3.modulation = SignedN<16>((sum << 5) >> (16 - s3.modLevel));
        } else {
            s3.modulation = 0;
        }
        const uint16 position = static_cast<uint16>(s3.reverse ? ~s3.currSample : s3.currSample);
        const sint32 phase = s3.reverse ? ~s3.currPhase : s3.currPhase;
        const sint32 fraction = ((phase >> 8) & 0x3F) + ((s3.modulation & 0x1F) << 1);
        const sint32 offset = SignedN<17>((position + SignedN<11>(s3.modulation >> 5) + (fraction >> 6)) & s3.mask);
        if (s3.pcm8Bit) {
            s3.sample1 = static_cast<sint16>(static_cast<sint8>(chip.m_WRAM[(s3.startAddress + offset) & 0x7FFFF]) << 8);
        } else {
            s3.sample1 = static_cast<sint16>(Be16(&chip.m_WRAM[((s3.startAddress & ~1u) + offset * 2) & 0x7FFFF]));
        }
    }
    Effects();

    if (s2.modLevel >= 5) {
        s2.modYSample = static_cast<sint16>(chip.m_soundStack[(chip.m_soundStackIndex - 1 + s2.modYSelect) & 63]);
    }
    Effects();

    Pitch(s1);
    Advance(s2);
    Fetch(s3);
    Envelope(s4);
    Level(s5);
    Attenuate(s6);
    Effects();

    // The finished voice into the output: directly, and what the DSP made of the inputs
    const uint32 finished = (i - 6) & 31;
    Mix(s7.output, s7.directSendLevel, s7.directPan);
    if (finished < 16) {
        Mix(dsp.effectOut[finished], s7.effectSendLevel, s7.effectPan);
    } else if (finished < 18) {
        Mix(dsp.audioInOut[finished & 1], s7.effectSendLevel, s7.effectPan);
    } else if (finished == 31) {
        Finish();
    }
    chip.m_soundStackIndex = (chip.m_soundStackIndex + 1) & 63;
    if (i == 31) {
        SampleEnd();
    }
}


// ---------------------------------------------------------------------------------------------------------
// The end of a sample: the timers count, interrupts are raised

void Chip::SampleEnd() {
    ++m_sampleCounter;
    for (uint32 i = 0; i < 3; i++) {
        Timer &timer = m_timers[i];
        if ((m_sampleCounter & timer.incrementMask) != 0) {
            continue;
        }
        // (a timer counts up from the value written to it and raises its interrupt on reaching 0xFF)
        if (timer.doReload) {
            timer.counter = timer.reload;
            timer.doReload = false;
        } else {
            timer.counter++;
        }
        if (timer.counter == 0xFF) {
            SetInterrupt(kIntrTimerA + i, true);
        }
    }
    SetInterrupt(kIntrSample, true);
    UpdateM68KInterrupts();
}

void Chip::SetInterrupt(uint16 intr, bool level) {
    m_m68kPendingInterrupts = static_cast<uint16>((m_m68kPendingInterrupts & ~(1u << intr)) | (level << intr));
    const uint16 before = m_scuPendingInterrupts;
    m_scuPendingInterrupts = static_cast<uint16>((m_scuPendingInterrupts & ~(1u << intr)) | (level << intr));
    if ((before ^ m_scuPendingInterrupts) & m_scuEnabledInterrupts & (1u << intr)) {
        UpdateSCUInterrupts();
    }
}

// The sound CPU's interrupt level: each interrupt's three level bits are in SCILV0-2 (interrupts 8 and up
// share bit 7)
void Chip::UpdateM68KInterrupts() {
    const uint16 active = m_m68kPendingInterrupts & m_m68kEnabledInterrupts;
    uint8 mask = static_cast<uint8>(active | ((active & ~0xFF) ? 0x80 : 0x00));
    uint8 level = 0;
    if (m_m68kInterruptLevels[2] & mask) {
        level |= 4;
        mask &= m_m68kInterruptLevels[2];
    }
    if (m_m68kInterruptLevels[1] & mask) {
        level |= 2;
        mask &= m_m68kInterruptLevels[1];
    }
    if (m_m68kInterruptLevels[0] & mask) {
        level |= 1;
    }
    m_m68kLevel = level;
    if (onSoundCpuInterrupt) {
        onSoundCpuInterrupt(level);
    }
}

void Chip::UpdateSCUInterrupts() {
    if (onMainInterrupt) {
        onMainInterrupt((m_scuPendingInterrupts & m_scuEnabledInterrupts) != 0);
    }
}

// The chip's DMA: words between sound RAM and its own registers
void Chip::ExecuteDMA() {
    while (m_dmaExec) {
        if (m_dmaXferToMem) {
            const uint16 value = Read(m_dmaRegAddress, true);
            PutBe16(&m_WRAM[m_dmaMemAddress & 0x7FFFE], m_dmaGate ? 0 : value);
        } else {
            const uint16 value = Be16(&m_WRAM[m_dmaMemAddress & 0x7FFFE]);
            Write(m_dmaRegAddress, m_dmaGate ? 0 : value, true);
        }
        m_dmaMemAddress = (m_dmaMemAddress + 2) & 0x7FFFE;
        m_dmaRegAddress = (m_dmaRegAddress + 2) & 0xFFE;
        m_dmaXferLength -= 2;
        if (m_dmaXferLength == 0) {
            m_dmaExec = false;
            SetInterrupt(kIntrDMATransferEnd, true);
            UpdateM68KInterrupts();
            UpdateSCUInterrupts();
        }
    }
}

uint32 Chip::ReceiveCDDA(const uint8 *data) {
    std::copy_n(data, 2352, m_cddaBuffer.begin() + m_cddaWritePos);
    m_cddaWritePos = (m_cddaWritePos + 2352) % m_cddaBuffer.size();
    sint32 length = static_cast<sint32>(m_cddaWritePos) - static_cast<sint32>(m_cddaReadPos);
    if (length < 0) {
        length += static_cast<sint32>(m_cddaBuffer.size());
    }
    if (length >= 2352 * 4) {
        m_cddaReady = true;
    }
    return static_cast<uint32>(length) * 3 / static_cast<uint32>(m_cddaBuffer.size());
}

// ---------------------------------------------------------------------------------------------------------
// Registers

Chip::Chip(uint8 *ram)
    : m_WRAM(ram) {
    m_dsp.m_WRAM = ram;
    Reset();
}

void Chip::Reset() {
    for (uint32 i = 0; i < 32; i++) {
        m_slots[i].Reset();
        m_slots[i].index = i;
    }
    m_dsp.Reset();
    m_soundStack.fill(0);
    m_soundStackIndex = 0;
    m_kyonex = m_kyonexExecute = false;
    m_masterVolume = 0;
    m_dac18Bits = m_mem4MB = false;
    m_monitorSlotCall = 0;
    m_timers = {};
    m_scuEnabledInterrupts = m_scuPendingInterrupts = 0;
    m_m68kEnabledInterrupts = m_m68kPendingInterrupts = 0;
    m_m68kInterruptLevels.fill(0);
    m_m68kLevel = 0;
    m_dmaExec = m_dmaXferToMem = m_dmaGate = false;
    m_dmaMemAddress = 0;
    m_dmaRegAddress = m_dmaXferLength = 0;
    m_sampleCounter = 0;
    m_lfsr = 1;
    m_out.fill(0);
    m_lastOut.fill(0);
    m_cddaBuffer.fill(0);
    m_cddaReadPos = m_cddaWritePos = 0;
    m_cddaReady = false;
}

void Slot::Reset() {
    // (the level the envelope showed last stays as it was: a reset does not touch it)
    const uint32 keep = index;
    const uint16 level = currEGLevel;
    *this = Slot{};
    index = keep;
    currEGLevel = level;
}

void Slot::CheckAttackBug() {
    if (keyRateScaling == 0xF) {
        egAttackBug = false;
    } else {
        const sint8 oct = static_cast<sint8>(static_cast<sint8>(octave ^ 8) - 8);
        const uint8 scaling = std::clamp<uint8>(static_cast<uint8>(keyRateScaling + oct), 0x0, 0xF);
        egAttackBug = (attackRate + scaling) >= 0x20;
    }
}

void Slot::UpdateMask() {
    mask = ~0u;
    if (maskMode) {
        mask = (loopEndAddress & 0x080) - 1;
        mask &= (loopEndAddress & 0x100) - 1;
        mask &= (loopEndAddress & 0x200) - 1;
        mask &= (loopEndAddress & 0x400) - 1;
    }
}

// A voice's settings from its register words. Rates in use follow the registers at once.
void Slot::Written(uint32 n, bool lower, bool upper) {
    static constexpr uint32 kLfoStepIntervals[32] = {1020, 892, 764, 636, 508, 444, 380, 316, 252, 220, 188, 156, 124, 108, 92, 76,
                                                     60,   52,  44,  36,  28,  24,  20,  16,  12,  10,  8,   6,   4,   3,   2,  1};
    const uint32 v = reg[n];
    auto bits = [v](unsigned lo, unsigned hi) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1u); };
    switch (n) {
    case 0x00 / 2:
        startAddress = (startAddress & 0xFFFF) | (bits(0, 3) << 16);
        pcm8Bit = bits(4, 4);
        loopControl = static_cast<LoopControl>(bits(5, 6));
        soundSource = static_cast<SoundSource>(bits(7, 8));
        if (upper) {
            static constexpr uint16 kSampleXOR[4] = {0x0000, 0x7FFF, 0x8000, 0xFFFF};
            sampleXOR = kSampleXOR[bits(9, 10)];
            keyOnBit = bits(11, 11);
        }
        break;
    case 0x02 / 2: startAddress = (startAddress & 0xF0000) | v; break;
    case 0x04 / 2: loopStartAddress = static_cast<uint16>(v); break;
    case 0x06 / 2:
        loopEndAddress = static_cast<uint16>(v);
        UpdateMask();
        break;
    case 0x08 / 2:
        attackRate = static_cast<uint8>(bits(0, 4));
        egHold = bits(5, 5);
        decay1Rate = static_cast<uint8>(bits(6, 10));
        decay2Rate = static_cast<uint8>(bits(11, 15));
        if (lower) {
            CheckAttackBug();
            if (egState == EGState::Attack) {
                currEGRate = attackRate;
            }
        }
        if (egState == EGState::Decay1) {
            currEGRate = decay1Rate;
        }
        if (upper && egState == EGState::Decay2) {
            currEGRate = decay2Rate;
        }
        break;
    case 0x0A / 2:
        releaseRate = static_cast<uint8>(bits(0, 4));
        decayLevel = static_cast<uint8>(bits(5, 9));
        keyRateScaling = static_cast<uint8>(bits(10, 13));
        loopStartLink = bits(14, 14);
        egBypass = bits(15, 15);
        if (lower && egState == EGState::Release) {
            currEGRate = releaseRate;
        }
        if (upper) {
            CheckAttackBug();
        }
        break;
    case 0x0C / 2:
        totalLevel = static_cast<uint8>(bits(0, 7));
        soundDirect = bits(8, 8);
        stackWriteInhibit = bits(9, 9);
        break;
    case 0x0E / 2:
        modYSelect = static_cast<uint8>(bits(0, 5));
        modXSelect = static_cast<uint8>(bits(6, 11));
        modLevel = static_cast<uint8>(bits(12, 15));
        break;
    case 0x10 / 2:
        freqNumSwitch = static_cast<uint16>(bits(0, 10) ^ 0x400u);
        octave = static_cast<uint8>(bits(11, 14));
        maskMode = bits(15, 15);
        if (upper) {
            UpdateMask();
            CheckAttackBug();
        }
        break;
    case 0x12 / 2:
        ampLFOSens = static_cast<uint8>(bits(0, 2));
        ampLFOWaveform = static_cast<Waveform>(bits(3, 4));
        pitchLFOSens = static_cast<uint8>(bits(5, 7));
        pitchLFOWaveform = static_cast<Waveform>(bits(8, 9));
        lfofRaw = static_cast<uint8>(bits(10, 14));
        lfoStepInterval = kLfoStepIntervals[lfofRaw];
        lfoReset = bits(15, 15);
        break;
    case 0x14 / 2:
        inputMixingLevel = static_cast<uint8>(bits(0, 2));
        inputSelect = static_cast<uint8>(bits(3, 6));
        break;
    case 0x16 / 2:
        effectPan = static_cast<uint8>(bits(0, 4));
        effectSendLevel = static_cast<uint8>(bits(5, 7));
        directPan = static_cast<uint8>(bits(8, 12));
        directSendLevel = static_cast<uint8>(bits(13, 15));
        break;
    default: break;
    }
}

uint16 Slot::Read(uint32 n) const {
    // (the bits that are kept: KYONEX and a few others read as zero)
    static constexpr uint16 kKept[12] = {0x0FFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0FFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00FF, 0xFFFF};
    return n < 12 ? static_cast<uint16>(reg[n] & kKept[n]) : 0;
}

void Dsp::Reset() {
    uint8 *ram = m_WRAM;
    *this = Dsp{};
    m_WRAM = ram;
}

// The program runs up to its last instruction that does something (and one more, for what that leaves to do)
void Dsp::UpdateProgramLength(uint8 writeIndex) {
    const bool wroteNothing = program[writeIndex].u64 == 0;
    if (wroteNothing && writeIndex == m_programLength - 1) {
        while (m_programLength > 0 && program[m_programLength - 1].u64 == 0) {
            --m_programLength;
        }
    } else if (!wroteNothing && writeIndex >= m_programLength) {
        m_programLength = static_cast<uint8>(writeIndex + 1);
    }
    if (m_programLength < program.size()) {
        ++m_programLength;
    }
}

namespace {
    // A byte or a word of a 16-bit register
    uint16 Part(uint16 value, uint32 address, bool word) {
        return word ? value : (address & 1) ? (value & 0xFF) : (value >> 8);
    }
    template <typename T>
    void Put(T &reg, uint32 address, uint16 value, bool word) {
        if (word) {
            reg = static_cast<T>(value);
        } else if (address & 1) {
            reg = static_cast<T>((reg & 0xFF00) | (value & 0xFF));
        } else {
            reg = static_cast<T>((reg & 0x00FF) | (value << 8));
        }
    }
} // namespace

uint16 Chip::Read(uint32 address, bool word) {
    address &= 0xFFF;
    const bool lower = word || (address & 1), upper = word || !(address & 1);
    if (address < 0x400) {
        return Part(m_slots[address >> 5].Read((address & 0x1F) >> 1), address, word);
    }
    if (address >= 0x600 && address < 0x680) {
        return Part(m_soundStack[(address >> 1) & 0x3F], address, word);
    }
    if (address >= 0x700 && address < 0x780) {
        return Part(static_cast<uint16>(m_dsp.coeffs[(address >> 1) & 0x3F] << 3), address, word);
    }
    if (address >= 0x780 && address < 0x7C0) {
        return Part(m_dsp.addrs[(address >> 1) & 0x1F], address, word);
    }
    if (address >= 0x7C0 && address < 0x800) {
        return 0;
    }
    if (address >= 0x800 && address < 0xC00) {
        return Part(m_dsp.program[(address >> 3) & 0x7F].u16[((address >> 1) & 3) ^ 3], address, word);
    }
    if (address >= 0xC00 && address < 0xEC0) {
        // (TEMP, MEMS: 24 bits as a word with the low 8 and a word with the high 16; MIXS: 20 bits as 4 and 16)
        const bool high = (address >> 1) & 1;
        uint32 value, lowBits = 8;
        if (address < 0xE00) {
            value = static_cast<uint32>(m_dsp.tempMem[(address >> 2) & 0x7F]);
        } else if (address < 0xE80) {
            value = static_cast<uint32>(m_dsp.soundMem[(address >> 2) & 0x1F]);
        } else {
            value = static_cast<uint32>(m_dsp.mixStack[(((address >> 2) & 0xF) | m_dsp.m_mixStackGen) ^ 0x10]);
            lowBits = 4;
        }
        const uint16 part = high ? static_cast<uint16>(value >> lowBits) : static_cast<uint16>(value & ((1u << lowBits) - 1));
        return Part(part, address, word);
    }
    if (address >= 0xEC0 && address < 0xEE0) {
        return Part(static_cast<uint16>(m_dsp.effectOut[(address >> 1) & 0xF]), address, word);
    }
    if (address >= 0xEE0 && address < 0xEE4) {
        return Part(static_cast<uint16>(m_dsp.audioInOut[(address >> 1) & 1]), address, word);
    }
    uint16 value = 0;
    switch (address & ~1u) {
    case 0x408: { // the monitored voice: envelope level and state, wave position
        const Slot &slot = m_slots[m_monitorSlotCall];
        value = static_cast<uint16>(((slot.currEGLevel >> 5) & 0x1F) | (static_cast<uint32>(slot.egState) << 5) | (((slot.currSample >> 12) & 0xF) << 7));
        break;
    }
    case 0x416:
        if (upper) {
            value = static_cast<uint16>((m_dmaExec << 12) | (m_dmaXferToMem << 13) | (m_dmaGate << 14));
        }
        break;
    case 0x41E: value = m_m68kEnabledInterrupts; break;
    case 0x420: value = m_m68kPendingInterrupts; break;
    case 0x424: value = m_m68kInterruptLevels[0]; break;
    case 0x426: value = m_m68kInterruptLevels[1]; break;
    case 0x428: value = m_m68kInterruptLevels[2]; break;
    case 0x42A: value = m_scuEnabledInterrupts; break;
    case 0x42C: value = m_scuPendingInterrupts; break;
    case 0x404: value = 0x0800 | 0x0100; break; // MIDI: nothing in, output empty
    default: break; // (the rest is write only)
    }
    (void)lower;
    return Part(value, address, word);
}

void Chip::Write(uint32 address, uint16 value, bool word) {
    address &= 0xFFF;
    const bool lower = word || (address & 1), upper = word || !(address & 1);
    // (a byte as it sits in its word)
    const uint16 value16 = word ? value : (address & 1) ? (value & 0xFF) : static_cast<uint16>(value << 8);
    if (address < 0x400) {
        Slot &slot = m_slots[address >> 5];
        const uint32 n = (address & 0x1F) >> 1;
        if (n < 12) {
            Put(slot.reg[n], address, value, word);
            slot.Written(n, lower, upper);
        }
        if (n == 0 && (value16 & 0x1000)) {
            m_kyonex = true; // KYONEX: every voice takes its key on bit at the start of the next sample
        }
        return;
    }
    if (address >= 0x600 && address < 0x680) {
        Put(m_soundStack[(address >> 1) & 0x3F], address, value, word);
        return;
    }
    if (address >= 0x700 && address < 0x780) {
        uint16 coefficient = static_cast<uint16>(m_dsp.coeffs[(address >> 1) & 0x3F] << 3);
        Put(coefficient, address, value, word);
        m_dsp.coeffs[(address >> 1) & 0x3F] = coefficient >> 3;
        return;
    }
    if (address >= 0x780 && address < 0x7C0) {
        Put(m_dsp.addrs[(address >> 1) & 0x1F], address, value, word);
        return;
    }
    if (address >= 0x7C0 && address < 0x800) {
        return;
    }
    if (address >= 0x800 && address < 0xC00) {
        const uint32 index = (address >> 3) & 0x7F;
        Put(m_dsp.program[index].u16[((address >> 1) & 3) ^ 3], address, value, word);
        m_dsp.UpdateProgramLength(static_cast<uint8>(index));
        return;
    }
    if (address >= 0xC00 && address < 0xEC0) {
        const bool high = (address >> 1) & 1;
        sint32 *target;
        uint32 lowBits = 8, bitsInAll = 24;
        if (address < 0xE00) {
            target = &m_dsp.tempMem[(address >> 2) & 0x7F];
        } else if (address < 0xE80) {
            target = &m_dsp.soundMem[(address >> 2) & 0x1F];
        } else {
            target = &m_dsp.mixStack[(((address >> 2) & 0xF) | m_dsp.m_mixStackGen) ^ 0x10];
            lowBits = 4;
            bitsInAll = 20;
        }
        const uint32 before = static_cast<uint32>(*target);
        if (!high) {
            uint16 part = static_cast<uint16>(before & ((1u << lowBits) - 1));
            Put(part, address, value, word);
            *target = static_cast<sint32>((before & ~((1u << lowBits) - 1)) | (part & ((1u << lowBits) - 1)));
        } else {
            uint16 part = static_cast<uint16>(before >> lowBits);
            Put(part, address, value, word);
            const uint32 fieldMask = ((1u << bitsInAll) - 1) & ~((1u << lowBits) - 1);
            uint32 after = (before & ~fieldMask) | ((static_cast<uint32>(part) << lowBits) & fieldMask);
            if (bitsInAll == 24) {
                after = static_cast<uint32>(Signed(after, 24));
            }
            *target = static_cast<sint32>(after);
        }
        return;
    }
    if (address >= 0xEC0 && address < 0xEE0) {
        Put(m_dsp.effectOut[(address >> 1) & 0xF], address, value, word);
        return;
    }
    if (address >= 0xEE0 && address < 0xEE4) {
        Put(m_dsp.audioInOut[(address >> 1) & 1], address, value, word);
        return;
    }
    switch (address & ~1u) {
    case 0x400:
        if (lower) {
            m_masterVolume = value16 & 0xF;
        }
        if (upper) {
            m_dac18Bits = value16 & 0x100;
            m_mem4MB = value16 & 0x200;
        }
        break;
    case 0x402:
        if (lower) {
            m_dsp.ringBufferLeadAddress = value16 & 0x7F;
            m_dsp.m_RBP = static_cast<uint32>(m_dsp.ringBufferLeadAddress) << 12;
        }
        {
            uint16 length = static_cast<uint16>(m_dsp.ringBufferLength << 7);
            if (lower) {
                length = (length & 0x100) | (value16 & 0x080);
            }
            if (upper) {
                length = (length & 0x080) | (value16 & 0x100);
            }
            m_dsp.ringBufferLength = static_cast<uint8>(length >> 7);
            m_dsp.m_RBL = (0x2000u << m_dsp.ringBufferLength) - 1u;
        }
        break;
    case 0x408:
        if (upper) {
            m_monitorSlotCall = static_cast<uint8>((value16 >> 11) & 0x1F);
        }
        break;
    case 0x412:
        if (lower) {
            m_dmaMemAddress = (m_dmaMemAddress & ~0xFEu) | (value16 & 0xFE);
        }
        if (upper) {
            m_dmaMemAddress = (m_dmaMemAddress & ~0xFF00u) | (value16 & 0xFF00);
        }
        break;
    case 0x414:
        if (lower) {
            m_dmaRegAddress = static_cast<uint16>((m_dmaRegAddress & ~0xFEu) | (value16 & 0xFE));
        }
        if (upper) {
            m_dmaRegAddress = static_cast<uint16>((m_dmaRegAddress & ~0xF00u) | (value16 & 0xF00));
            m_dmaMemAddress = (m_dmaMemAddress & 0xFFFF) | (static_cast<uint32>(value16 >> 12) << 16);
        }
        break;
    case 0x416:
        if (lower) {
            m_dmaXferLength = static_cast<uint16>((m_dmaXferLength & ~0xFEu) | (value16 & 0xFE));
        }
        if (upper) {
            m_dmaXferLength = static_cast<uint16>((m_dmaXferLength & ~0xF00u) | (value16 & 0xF00));
            m_dmaExec = m_dmaExec || (value16 & 0x1000);
            m_dmaXferToMem = value16 & 0x2000;
            m_dmaGate = value16 & 0x4000;
            ExecuteDMA();
        }
        break;
    case 0x418:
    case 0x41A:
    case 0x41C: {
        Timer &timer = m_timers[((address & ~1u) - 0x418) >> 1];
        if (lower) {
            timer.reload = static_cast<uint8>(value16);
            timer.doReload = true;
        }
        if (upper) {
            timer.incrementInterval = (value16 >> 8) & 7;
            timer.incrementMask = (1ull << timer.incrementInterval) - 1;
        }
        break;
    }
    case 0x41E: {
        uint16 enabled = m_m68kEnabledInterrupts;
        Put(enabled, address, value, word);
        m_m68kEnabledInterrupts = enabled & 0x7FF;
        UpdateM68KInterrupts();
        break;
    }
    case 0x420:
        if (lower) {
            m_m68kPendingInterrupts = static_cast<uint16>((m_m68kPendingInterrupts & ~0x20u) | (value16 & 0x20));
            UpdateM68KInterrupts();
        }
        break;
    case 0x422:
        m_m68kPendingInterrupts &= ~value16;
        UpdateM68KInterrupts();
        break;
    case 0x424:
    case 0x426:
    case 0x428:
        if (lower) {
            m_m68kInterruptLevels[((address & ~1u) - 0x424) >> 1] = static_cast<uint8>(value16);
            UpdateM68KInterrupts();
        }
        break;
    case 0x42A: {
        uint16 enabled = m_scuEnabledInterrupts;
        Put(enabled, address, value, word);
        m_scuEnabledInterrupts = enabled & 0x7FF;
        UpdateSCUInterrupts();
        break;
    }
    case 0x42C:
        if (lower) {
            m_scuPendingInterrupts = static_cast<uint16>((m_scuPendingInterrupts & ~0x20u) | (value16 & 0x20));
            UpdateSCUInterrupts();
        }
        break;
    case 0x42E:
        m_scuPendingInterrupts &= ~value16;
        UpdateSCUInterrupts();
        break;
    default: break;
    }
}

} // namespace satrecomp::sound
