// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The Saturn's sound chip (SCSP): 32 voices ("slots") playing waves from sound RAM with an envelope, LFOs and
// FM, an effects processor (DSP), three timers and the interrupts to the sound CPU and the main CPUs.
//
// The machine writes and reads its registers (0x000-0xEE3 of the chip's address space) and calls Step for each
// of a sample's 32 steps. A step moves seven voices one stage each through the chip's pipeline (key on/off
// and pitch, the wave's address, reading wave memory, interpolation and the envelope, level, output) and
// runs four instructions of the DSP; after the 32nd the sample goes out, the timers count and interrupts are
// raised. Nothing here knows which machine model drives it (docs/SOUND.md).
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace satrecomp {
class Archive;
}

namespace satrecomp::sound {

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint8 = int8_t;
using sint16 = int16_t;
using sint32 = int32_t;
using sint64 = int64_t;

// A voice: what its registers say (the first group) and where it is in its wave and envelope
struct Slot {
    uint32 index = 0;
    uint16 reg[12] = {}; // its registers as written (0x00-0x16)

    uint32 startAddress = 0;     // SA: where its wave starts in sound RAM
    uint16 loopStartAddress = 0; // LSA
    uint16 loopEndAddress = 0;   // LEA
    bool pcm8Bit = false;        // PCM8B: 8-bit samples
    bool keyOnBit = false;       // KYONB: wants to play (taken when KYONEX is written)
    enum class LoopControl { Off, Normal, Reverse, Alternate };
    LoopControl loopControl = LoopControl::Off; // LPCTL
    uint16 sampleXOR = 0;                       // SBCTL: bits of every sample turned over
    enum class SoundSource { SoundRAM, Noise, Silence, Unknown };
    SoundSource soundSource = SoundSource::SoundRAM; // SSCTL
    uint8 attackRate = 0, decay1Rate = 0, decay2Rate = 0, releaseRate = 0; // AR, D1R, D2R, RR
    uint8 decayLevel = 0;                                                  // DL
    uint8 keyRateScaling = 0;                                              // KRS (15: none)
    bool egHold = false, loopStartLink = false, egBypass = false;          // EGHOLD, LPSLNK, (undocumented)
    uint8 modLevel = 0, modXSelect = 0, modYSelect = 0;                    // MDL, MDXSL, MDYSL: FM from other voices
    bool stackWriteInhibit = false;                                        // STWINH
    uint8 totalLevel = 0;                                                  // TL
    bool soundDirect = false;                                              // SDIR: no envelope or level
    uint8 octave = 0;                                                      // OCT
    uint16 freqNumSwitch = 0x400;                                          // FNS (with its implied bit 10)
    bool maskMode = false;                                                 // MSK (undocumented)
    uint32 mask = ~0u;                                                     // ... the wave address mask it gives
    enum class Waveform : uint8 { Saw, Square, Triangle, Noise };
    bool lfoReset = false;                                      // LFORE
    uint8 lfofRaw = 0;                                          // LFOF
    uint32 lfoStepInterval = 1020;                              // ... as samples per LFO step
    uint8 ampLFOSens = 0, pitchLFOSens = 0;                     // ALFOS, PLFOS
    Waveform ampLFOWaveform = Waveform::Saw, pitchLFOWaveform = Waveform::Saw;
    uint8 inputMixingLevel = 0, inputSelect = 0;                // IMXL, ISEL: into the DSP
    uint8 directSendLevel = 0, directPan = 0;                   // DISDL, DIPAN
    uint8 effectSendLevel = 0, effectPan = 0;                   // EFSDL, EFPAN

    bool active = false;
    enum class EGState : uint8 { Attack, Decay1, Decay2, Release };
    EGState egState = EGState::Release;
    uint16 egLevel = 0x3FF;
    uint8 currEGRate = 0;
    uint16 currEGLevel = 0;
    bool egAttackBug = false; // (a hardware quirk: with some rates the attack never ends)
    uint32 currSample = 0, currPhase = 0, nextPhase = 0;
    sint16 modXSample = 0, modYSample = 0;
    sint32 modulation = 0;
    bool reverse = false, crossedLoopStart = false;
    uint32 lfoCycles = 0;
    uint8 lfoStep = 0, alfoOutput = 0;
    sint16 sample1 = 0, sample2 = 0, output = 0;
    sint32 finalLevel = 0;

    void Reset();
    // A register byte or word was written: `lower`/`upper` say which bytes of register word `n` (0..11)
    void Written(uint32 n, bool lower, bool upper);
    uint16 Read(uint32 n) const;

private:
    void CheckAttackBug();
    void UpdateMask();
};

// One instruction of the DSP's program
union DspInstr {
    uint64 u64;
    uint16 u16[4];
    struct {
        uint64 NXADR : 1; // the memory address plus one
        uint64 ADREB : 1; // add the address register
        uint64 MASA : 5;  // which address (MADRS)
        uint64 : 1;
        uint64 NOFL : 1; // wave memory as it is (not as floating point)
        uint64 CRA : 6;  // which coefficient
        uint64 : 1;
        uint64 BSEL : 1;  // add the accumulator (not TEMP)
        uint64 ZERO : 1;  // add nothing
        uint64 NEGB : 1;  // subtract
        uint64 YRL : 1;   // latch the input into Y
        uint64 SHFT0 : 1; // the shifter
        uint64 SHFT1 : 1;
        uint64 FRCL : 1; // latch the fraction
        uint64 ADRL : 1; // latch the address
        uint64 EWA : 4;  // which effect output
        uint64 EWT : 1;  // ... written
        uint64 MRD : 1;  // read wave memory
        uint64 MWT : 1;  // write wave memory
        uint64 TABLE : 1; // the address is not in the ring buffer
        uint64 IWA : 5;   // which input is written
        uint64 IWT : 1;
        uint64 IRA : 6; // which input is read
        uint64 : 1;
        uint64 YSEL : 2; // the multiplier's Y: fraction, coefficient, or parts of Y
        uint64 XSEL : 1; // the multiplier's X: the input (not TEMP)
        uint64 TWA : 7;  // which TEMP is written
        uint64 TWT : 1;
        uint64 TRA : 7; // which TEMP is read
        uint64 : 1;
    };
    struct {
        uint64 : 20;
        uint64 SHFT : 2;
    };
};

struct Dsp {
    std::array<DspInstr, 128> program{}; // MPRO
    std::array<sint32, 128> tempMem{};   // TEMP (24 bits)
    std::array<sint32, 32> soundMem{};   // MEMS (24 bits)
    std::array<uint16, 64> coeffs{};     // COEF (13 bits)
    std::array<uint16, 32> addrs{};      // MADRS
    std::array<sint32, 32> mixStack{};   // MIXS: the voices' sums, two sets (one filled while the other is read)
    std::array<sint16, 16> effectOut{};  // EFREG
    std::array<sint16, 2> audioInOut{};  // EXTS: CD audio in
    uint8 ringBufferLeadAddress = 0, ringBufferLength = 0; // RBP, RBL
    uint8 PC = 0x68; // (the program is in step with the pipeline's last stage, six voices behind)
    uint32 m_RBP = 0, m_RBL = 0x1FFF;
    uint8 m_programLength = 0;
    sint32 INPUTS = 0;
    uint32 SFT_REG = 0;
    uint16 FRC_REG = 0;
    uint32 Y_REG = 0;
    uint16 ADRS_REG = 0;
    uint16 MDEC_CT = 0;
    uint8 m_mixStackGen = 0;
    uint16 m_mixStackNull = 0xFFFF;
    bool m_readPending = false, m_readNOFL = false;
    uint32 m_readValue = 0;
    bool m_writePending = false;
    uint16 m_writeValue = 0;
    uint32 m_readWriteAddr = 0;
    uint8 *m_WRAM = nullptr;

    void Reset();
    void UpdateProgramLength(uint8 writeIndex);
};

struct Timer {
    uint8 incrementInterval = 0, reload = 0, counter = 0;
    uint64 incrementMask = 0;
    bool doReload = false;
};

// Interrupt numbers (bits of the pending and enable registers)
enum : uint16 { kIntrDMATransferEnd = 4, kIntrCPUManual = 5, kIntrTimerA = 6, kIntrSample = 10 };

class Chip {
public:
    // `ram`: sound RAM, 512 KiB, which the machine keeps (the sound CPU's memory)
    explicit Chip(uint8 *ram);
    void Reset();

    // The chip's registers, by address within the chip (0x000-0xFFF), a byte or a word at a time
    uint16 Read(uint32 address, bool word);
    void Write(uint32 address, uint16 value, bool word);

    // Step i (0..31) of a sample. After step 31 the sample has gone out and SampleEnd has run.
    void Step(uint32 i);

    void Serialize(Archive &ar); // save states (runtime/state.h)

    // A sector of CD audio from the drive (2352 bytes); how full the buffer is, in thirds
    uint32 ReceiveCDDA(const uint8 *data);

    // What the chip tells the machine
    std::function<void(sint16 left, sint16 right)> onSample;
    std::function<void(uint8 level)> onSoundCpuInterrupt; // the level on the sound CPU's interrupt lines
    std::function<void(bool level)> onMainInterrupt;      // the sound request line to the main CPUs' side
    // CD audio from somewhere other than the chip's own buffer: asked for every sample; when it has one, that
    // is the CD audio input
    bool (*cdAudioSource)(sint16 &left, sint16 &right) = nullptr;
    bool linearInterpolation = true;
    bool emit = true; // (false: samples are made but onSample is not called)

    // --- state (public for the generation code's helpers and for comparison with a reference) ---
    uint8 *m_WRAM;
    std::array<Slot, 32> m_slots;
    Dsp m_dsp;
    std::array<uint16, 64> m_soundStack{}; // SOUS: the voices' last outputs, for FM
    uint32 m_soundStackIndex = 0;
    bool m_kyonex = false, m_kyonexExecute = false;
    uint32 m_masterVolume = 0;
    bool m_dac18Bits = false, m_mem4MB = false;
    uint8 m_monitorSlotCall = 0;
    std::array<Timer, 3> m_timers;
    uint16 m_scuEnabledInterrupts = 0, m_scuPendingInterrupts = 0;   // MCIEB, MCIPD
    uint16 m_m68kEnabledInterrupts = 0, m_m68kPendingInterrupts = 0; // SCIEB, SCIPD
    std::array<uint8, 3> m_m68kInterruptLevels{};                    // SCILV0-2
    uint8 m_m68kLevel = 0;                                           // (what onSoundCpuInterrupt was told last)
    bool m_dmaExec = false, m_dmaXferToMem = false, m_dmaGate = false;
    uint32 m_dmaMemAddress = 0;
    uint16 m_dmaRegAddress = 0, m_dmaXferLength = 0;
    uint64 m_sampleCounter = 0;
    uint32 m_lfsr = 1;
    std::array<sint32, 2> m_out{};
    std::array<sint32, 2> m_lastOut{}; // the sample that went out last
    std::array<uint8, 2352 * 15> m_cddaBuffer{};
    uint32 m_cddaReadPos = 0, m_cddaWritePos = 0;
    bool m_cddaReady = false;

private:
    bool Trigger(Slot &slot);
    void Pitch(Slot &slot);     // stage 1: key on/off, the pitch LFO, the phase
    void Advance(Slot &slot);   // stage 2: the wave position, loops
    void Fetch(Slot &slot);     // stage 3: modulation and the two samples around the position
    void Envelope(Slot &slot);  // stage 4: interpolation, the amplitude LFO, the envelope generator
    void Level(Slot &slot);     // stage 5
    void Attenuate(Slot &slot); // stage 6
    void Effects();             // one instruction of the DSP's program
    void Mix(sint32 output, uint8 sendLevel, uint8 pan);
    void Finish();
    void SampleEnd();

    void SetInterrupt(uint16 intr, bool level);
    void UpdateM68KInterrupts();
    void UpdateSCUInterrupts();
    void ExecuteDMA();
};

} // namespace satrecomp::sound
