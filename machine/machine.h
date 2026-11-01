// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// satrecomp's own Saturn: just enough of the machine for the game's code to run on, not a model of the
// hardware's timing. Two SH-2s (sh2.h), their memory, the system control unit (interrupts, timers, DMA), the
// system manager (SMPC: CPU on/off, clock change, pads), the video chips' registers and timing (the picture is
// made by video/renderer.h), the sound chip and its CPU (sound/chip.h, sound/m68k.h) and what the game still
// asks of the CD drive.
//
// Time is counted in SH-2 clock cycles. Run() lets the CPUs run up to the next thing due in the hardware
// (the end of a part of a video line, a sound sample, ...) and then does that.
#pragma once

#include "sh2.h"

#include "../sound/chip.h"
#include "../sound/m68k.h"
#include "../video/renderer.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace satrecomp::machine {

// The game disc, as far as the CD drive's registers need it
struct Disc {
    struct Track {
        uint8 number = 0;       // 1..99
        uint8 control = 0;      // 4: data, 0: audio
        uint32 startFAD = 0;    // frame address of its index 1
    };
    std::vector<Track> tracks;
    uint32 leadOutFAD = 0;
};

class Machine final : private sound::M68KBus {
public:
    Machine();
    ~Machine();

    // Power on (hard) or the reset the SMPC does
    void Reset(bool hard);
    // Runs until the picture of a frame is complete
    void RunFrame();

    // The ROM area (512 KiB): the port puts the BIOS's vectors there (runtime/hle_bios.cpp)
    void LoadRom(const uint8 *data, size_t size);
    // Code (work RAM addresses lo..hi) that needs the two CPUs to alternate in short slices whenever either runs
    // it, as the hardware runs them side by side (game.cfg fine_slices: Fighters Megamix's movie player)
    void SetFineSlices(uint32 lo, uint32 hi) {
        m_fineLo = lo & 0x07FFFFFF;
        m_fineHi = hi & 0x07FFFFFF;
    }
    void SetDisc(const Disc &disc) {
        m_disc = disc;
    }
    // The buttons a player holds (Saturn control pad layout, 1 = pressed), asked when the game reads the pads
    std::function<uint16(int player)> readPad;
    // Seconds since 1970, for the SMPC's clock (asked when the game reads it)
    std::function<int64_t()> readClock;

    SH2 masterSH2, slaveSH2;
    bool slaveEnabled = false;
    uint8 *ramHigh = nullptr; // 1 MiB at 0x06000000
    uint8 *ramLow = nullptr;  // 1 MiB at 0x00200000
    uint8 *rom = nullptr;     // 512 KiB at 0x00000000
    static constexpr size_t kRamSize = 0x100000, kRomSize = 0x80000;

    video::Renderer &Video() {
        return *m_video;
    }
    sound::Chip &Sound() {
        return *m_sound;
    }
    uint8 *SoundRam() {
        return m_soundRam.data();
    }
    uint64 Now() const {
        return m_now;
    }
    // Save states: everything of the machine that a later moment depends on (runtime/state.h)
    void Serialize(Archive &ar);
    // What the CPUs have asked of the SMPC and the CD drive (for --stats)
    uint64 smpcCommands[256] = {};
    uint64 cdCommands[256] = {};

    // --- the bus, as the SH-2s see it (27-bit addresses) ---
    uint8 BusRead8(uint32 address);
    uint16 BusRead16(uint32 address);
    uint32 BusRead32(uint32 address);
    void BusWrite8(uint32 address, uint8 value);
    void BusWrite16(uint32 address, uint16 value);
    void BusWrite32(uint32 address, uint32 value);
    // A CPU has taken the interrupt on its external line
    void AcknowledgeExternalInterrupt(SH2 &cpu);

private:
    std::vector<uint8> m_ramHigh, m_ramLow, m_rom, m_soundRam;
    uint64 m_now = 0;
    uint64 m_masterSpillover = 0, m_slaveSpillover = 0;
    bool m_frameDone = false;
    // After one CPU has signalled the other, they alternate in short slices for this long
    static constexpr uint64 kFineCyclesAfterSignal = 20000;
    uint64 m_fineCycles = 0;
    uint32 m_fineLo = 0, m_fineHi = 0; // code where the two CPUs always alternate in short slices (SetFineSlices)
    uint32 m_clock = 26846591; // SH-2 cycles a second (changes with the dot clock: 320 or 352 dots a line)
    bool m_clock352 = false;

    void Run();
    void SetClock(bool dots352);

    // --- system control unit (0x05FE0000) ---
    struct DmaLevel {
        uint32 source = 0, target = 0, count = 0;
        uint32 sourceAdd = 4, targetAdd = 2;
        bool enabled = false, indirect = false, updateSource = false, updateTarget = false;
        uint8 trigger = 7; // 0 VBlank in, 1 VBlank out, 2 HBlank in, 3 timer 0, 4 timer 1, 5 sound, 6 sprites drawn, 7 at once
        uint32 raw[6] = {};
    };
    struct {
        uint32 status = 0;    // interrupts raised and not yet taken (bit n: vector 0x40 + n)
        uint32 mask = 0xBFFF; // ... and those held back
        uint8 pendingLevel = 0;
        DmaLevel dma[3];
        bool timerEnable = false, timer1Mode = false;
        uint16 timer0Counter = 0, timer0Compare = 0, timer1Reload = 0;
        uint64 timer1Due = 0; // (0: not running)
        uint32 registers[0x40] = {};
    } m_scu;
    void ScuReset();
    uint32 ScuRead(uint32 address);
    void ScuWrite(uint32 address, uint32 value);
    void ScuRaise(unsigned interrupt);
    void ScuUpdateInterrupt();
    void ScuTriggerDma(uint8 trigger);
    void ScuRunDma(unsigned level);

    // --- system manager and peripherals (0x00100000) ---
    struct {
        uint8 IREG[7] = {}, OREG[32] = {}, command = 0, SR = 0x80;
        bool SF = false;
        uint8 PDR[2] = {}, DDR[2] = {}, IOSEL = 0, EXLE = 0;
        uint64 commandDue = 0; // (0: none running)
        bool intbackInProgress = false, optimize = false, getPeripheralData = false, resetDisable = true;
        uint8 portMode[2] = {};
        uint8 report[32] = {}; // the peripherals' data being handed out
        uint32 reportSize = 0, reportOffset = 0;
        uint8 SMEM[4] = {};
    } m_smpc;
    uint8 SmpcRead(uint32 address);
    void SmpcWrite(uint32 address, uint8 value);
    void SmpcCommand();
    void SmpcStatusReport();
    void SmpcPeripheralReport();

    // --- video chips (0x05C00000 VDP1, 0x05E00000 VDP2) ---
    std::vector<uint8> m_vram1, m_fb[2], m_vram2, m_cram;
    std::unique_ptr<video::Renderer> m_video;
    struct {
        uint16 regs2[0x100] = {}; // the VDP2's registers as written
        uint16 TVSTAT = 0, VCNT = 0;
        bool hblank = false, vblank = false, odd = true;
        uint8 hPhase = 0, vPhase = 0; // where in the line and in the frame
        uint32 hTimings[4] = {1280, 216, 104, 108};
        uint32 vTimings[2][6] = {{224, 232, 237, 255, 262, 263}, {224, 232, 237, 255, 261, 262}};
        uint32 field = 0; // which of the two rows of vTimings
        uint32 width = 320, height = 224;
        bool resolutionDirty = true;
        uint64 phaseDue = 0;
        // VDP1
        uint16 TVMR = 0, FBCR = 0, PTMR = 0, EWDR = 0, EWLR = 0, EWRR = 0;
        uint16 EDSR = 0, LOPR = 0, COPR = 0;
        bool swapMode = false, swapTrigger = false, paramsChanged = false, vblankErase = false;
        bool doVBlankErase = false, doDisplayErase = false;
        bool frameEnded = false, previousFrameEnded = false;
        uint32 commandAddress = 0, previousCommandAddress = 0;
        uint64 drawEndDue = 0;
    } m_vdp;
    void VdpReset();
    uint16 Vdp1ReadRegister(uint32 address);
    void Vdp1WriteRegister(uint32 address, uint16 value);
    uint16 Vdp2ReadRegister(uint32 address);
    void Vdp2WriteRegister(uint32 address, uint16 value);
    void VdpUpdateResolution();
    void VdpPhase();
    void VdpLineEnd();
    void Vdp1Swap();
    void Vdp1Draw();

    // --- sound (0x05A00000 sound RAM, 0x05B00000 the chip) ---
    std::unique_ptr<sound::Chip> m_sound;
    sound::M68K m_soundCpu{*this};
    bool m_soundCpuEnabled = false;
    uint64 m_soundCpuSpillover = 0;
    uint64 m_sampleDue = 0, m_sampleFraction = 0;
    void SoundSample();
    uint8 Read8(uint32 address) override; // (the sound CPU's bus)
    uint16 Read16(uint32 address) override;
    void Write8(uint32 address, uint8 value) override;
    void Write16(uint32 address, uint16 value) override;

    // --- CD drive (0x05890000) ---
    Disc m_disc;
    struct {
        uint16 HIRQ = 0x0BE1, HIRQMask = 0, CR[4] = {0x0043, 0x4442, 0x4C4F, 0x434B}; // ("CDBLOCK")
        uint16 command[4] = {};
        uint8 status = 0x01;  // 0 busy, 1 pause, 2 standby, 3 play, 4 seek, ...
        uint32 fad = 150;     // where the drive's pickup is
        uint32 playEnd = 0;
        uint32 buffered = 0;  // sectors read into the buffer and not taken
        uint32 bufferFAD[200] = {}; // ... their frame addresses, oldest first
        bool reading = false;
        bool responseTaken = true; // the last command's response has been read
        uint16 transfer[204] = {}; // words waiting at the data port: the TOC, or
        uint32 transferFAD[200] = {}; // sectors from the buffer, by frame address (transferSectors of them)
        uint32 transferSectors = 0;
        uint32 transferSize = 0, transferOffset = 0; // in words
        uint32 actualSize = 0; // words, as worked out by "calculate actual size"
        uint64 periodicDue = 0;
    } m_cd;
    void CdReset();
    uint16 CdRead(uint32 address);
    void CdWrite(uint32 address, uint16 value);
    void CdCommand();
    void CdReport(uint8 status);
    void CdPeriodic();
    void CdSectorRange(uint32 &first, uint32 &count);
};

} // namespace satrecomp::machine
