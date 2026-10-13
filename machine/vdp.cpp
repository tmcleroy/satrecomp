// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The video chips' registers and timing. What they show is the renderer's business (video/renderer.h); here
// is when: the parts of a line and of a frame, the interrupts at the blanking periods, the VDP1 going through
// its command table and swapping its framebuffers.
#include "machine.h"

namespace satrecomp::machine {

namespace {
    // The parts of a line: the picture, the right border, the sync pulse, the left border. Of a frame: the
    // picture, the bottom border, blanking and sync, the lines the counter skips, the top border, the last line.
    enum : uint8 { kHActive, kHRightBorder, kHSync, kHLeftBorder };
    enum : uint8 { kVActive, kVBottomBorder, kVBlanking, kVCounterSkip, kVTopBorder, kVLastLine };

    uint16 Get16(const uint8 *p) {
        return static_cast<uint16>((p[0] << 8) | p[1]);
    }
} // namespace

void Machine::VdpReset() {
    m_vdp = {};
    m_video->Reset(true);
    for (uint32 address = 0; address < 0x200; address += 2) {
        m_video->WriteVdp2Reg(address, 0);
    }
    m_video->SetField(m_vdp.odd);
    VdpUpdateResolution();
    m_vdp.phaseDue = m_now + m_vdp.hTimings[kHActive];
}

// The picture's size and the lengths of the parts of a line and a frame, from the display mode register
void Machine::VdpUpdateResolution() {
    if (!m_vdp.resolutionDirty) {
        return;
    }
    m_vdp.resolutionDirty = false;
    const uint16 mode = m_vdp.regs2[0];
    const uint32 hreso = mode & 7, vreso = (mode >> 4) & 3, interlace = (mode >> 6) & 3;
    const bool exclusive = hreso & 4, interlaced = interlace >= 2;
    static constexpr uint32 kWidths[4] = {320, 352, 640, 704};
    m_vdp.width = kWidths[hreso & 3];
    m_vdp.height = exclusive ? 480 : (vreso & 1) ? 240 : 224;
    if (!exclusive && interlaced) {
        m_vdp.height *= 2;
    }
    // (in dots; a dot is four clock cycles, two in the high resolution modes)
    static constexpr uint32 kLine[8][4] = {{320, 54, 26, 27}, {352, 51, 29, 23}, {640, 108, 52, 54}, {704, 102, 58, 46},
                                           {160, 27, 13, 13}, {176, 11, 13, 12}, {320, 54, 26, 26},  {352, 22, 26, 24}};
    const uint32 cyclesPerDot = (hreso & 2) ? 2 : 4;
    for (int i = 0; i < 4; i++) {
        m_vdp.hTimings[i] = kLine[hreso][i] * cyclesPerDot;
    }
    static constexpr uint32 kFrame[2][2][6] = {{{224, 232, 237, 255, 262, 263}, {224, 232, 237, 255, 261, 262}},
                                               {{240, 240, 245, 255, 262, 263}, {240, 240, 245, 255, 261, 262}}};
    static constexpr uint32 kFrameExclusive[2][6] = {{480, 496, 506, 509, 524, 525}, {480, 496, 506, 509, 526, 527}};
    for (int field = 0; field < 2; field++) {
        for (int i = 0; i < 6; i++) {
            m_vdp.vTimings[field][i] = exclusive ? kFrameExclusive[field][i] : kFrame[vreso & 1][field][i];
        }
    }
    m_vdp.field = interlaced && m_vdp.odd ? 1 : 0;
    m_video->SetResolution(m_vdp.width, m_vdp.height, exclusive);
}

// The next part of the line begins
void Machine::VdpPhase() {
    auto &v = m_vdp;
    v.hPhase = (v.hPhase + 1) & 3;
    switch (v.hPhase) {
    case kHActive:
        if (v.vPhase == kVActive) {
            if (v.VCNT == v.vTimings[v.field][kVActive] - 16) {
                // (about a millisecond before the picture ends: the pads are read, if the game asked for that)
                if (m_smpc.optimize) {
                    m_smpc.optimize = false;
                    if (m_smpc.SF && m_smpc.intbackInProgress && m_smpc.getPeripheralData) {
                        m_smpc.commandDue = m_now ? m_now : 1;
                    }
                }
            }
            m_video->RenderLine(v.VCNT);
        }
        break;
    case kHRightBorder:
        v.hblank = true;
        ScuRaise(2); // HBlank in
        if (m_scu.timerEnable) {
            m_scu.timer0Counter = (m_scu.timer0Counter + 1) & 0x1FF;
            if (m_scu.timer0Counter == m_scu.timer0Compare) {
                ScuRaise(3);
                ScuTriggerDma(3);
            }
            if (!m_scu.timer1Due) {
                m_scu.timer1Due = m_now + (m_scu.timer1Reload + 1u) * 4;
            }
        }
        ScuTriggerDma(2);
        if (!v.vblank) {
            slaveSH2.SetExternalInterrupt(2, 0x41);
        }
        if (v.VCNT == v.vTimings[v.field][kVActive]) {
            // (the picture's lines are over: the field changes)
            v.doVBlankErase = v.vblankErase;
            const uint32 interlace = (v.regs2[0] >> 6) & 3;
            if (interlace != 0) {
                v.odd = !v.odd;
                v.field = v.odd;
                m_video->SetField(v.odd);
            } else if (!v.odd) {
                v.odd = true;
                v.field = 0;
                m_video->SetField(v.odd);
            }
        }
        break;
    case kHSync: break;
    case kHLeftBorder:
        if (v.vPhase == kVLastLine) {
            // The frame is about to start: the VDP1 erases and swaps its framebuffers, as its registers say
            bool erase = false, swap = false;
            if (!v.swapMode) {
                erase = swap = true;
            } else if (v.paramsChanged) {
                if (v.swapTrigger) {
                    swap = true;
                } else {
                    erase = true;
                }
            }
            v.paramsChanged = false;
            if (v.doVBlankErase) {
                const uint64 lineCycles = v.hTimings[0] + v.hTimings[1] + v.hTimings[2] + v.hTimings[3];
                m_video->EraseFramebuffer((lineCycles - 113) * (v.vTimings[v.field][kVLastLine] - v.vTimings[v.field][kVActive]));
            }
            if (erase) {
                v.doDisplayErase = true;
            }
            if (swap) {
                Vdp1Swap();
            }
        }
        v.hblank = false;
        if (v.vPhase == kVActive && !v.vblank) {
            slaveSH2.SetExternalInterrupt(0, 0);
        }
        VdpLineEnd();
        break;
    }
    v.phaseDue += v.hTimings[v.hPhase];
}

// The line counter moves on, and with it the parts of the frame
void Machine::VdpLineEnd() {
    auto &v = m_vdp;
    ++v.VCNT;
    while (v.VCNT >= v.vTimings[v.field][v.vPhase]) {
        uint8 next = v.vPhase + 1;
        if (next == 6) {
            v.VCNT = 0;
            next = 0;
        }
        v.vPhase = next;
        switch (v.vPhase) {
        case kVBottomBorder: // VBlank in
            v.vblank = true;
            ScuRaise(0);
            ScuTriggerDma(0);
            slaveSH2.SetExternalInterrupt(6, 0x43);
            m_smpc.intbackInProgress = false; // (a pad read not finished by now is given up)
            break;
        case kVBlanking:
            m_video->EndFrame();
            m_frameDone = true;
            if (v.doDisplayErase) {
                v.doDisplayErase = false;
                m_video->EraseFramebuffer(0);
            }
            break;
        case kVTopBorder:
            VdpUpdateResolution();
            m_video->LatchDisplayMode();
            break;
        case kVLastLine: // VBlank out
            m_video->BeginFrame();
            v.vblank = false;
            ScuRaise(1);
            m_scu.timer0Counter = 0;
            if (m_scu.timerEnable && m_scu.timer0Compare == 0) {
                ScuRaise(3);
            }
            ScuTriggerDma(1);
            slaveSH2.SetExternalInterrupt(4, 0x42);
            break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// VDP2 registers

uint16 Machine::Vdp2ReadRegister(uint32 address) {
    switch (address) {
    case 0x000: return m_vdp.regs2[0];
    case 0x002: return m_vdp.regs2[1];
    case 0x004: // TVSTAT: the blanking periods and the field
        return static_cast<uint16>(0x0200 | (m_vdp.vblank << 3) | (m_vdp.hblank << 2) | (m_vdp.odd << 1));
    case 0x006: return m_vdp.regs2[3];
    case 0x008: return 0; // HCNT
    case 0x00A: { // VCNT (in a double density picture it counts the frame's lines)
        const uint32 interlace = (m_vdp.regs2[0] >> 6) & 3;
        return static_cast<uint16>(interlace == 3 ? (m_vdp.VCNT << 1) | !m_vdp.odd : m_vdp.VCNT);
    }
    case 0x00E: return m_vdp.regs2[7];
    default: return 0; // (the rest is write only)
    }
}

void Machine::Vdp2WriteRegister(uint32 address, uint16 value) {
    if (address == 0 && m_vdp.regs2[0] != value) {
        m_vdp.resolutionDirty = true; // (the display mode changes with the next frame)
    }
    m_vdp.regs2[address >> 1] = value;
    m_video->WriteVdp2Reg(address, value);
}

// ---------------------------------------------------------------------------------------------------------
// VDP1

uint16 Machine::Vdp1ReadRegister(uint32 address) {
    switch (address) {
    case 0x10: return static_cast<uint16>((m_vdp.frameEnded << 1) | m_vdp.previousFrameEnded); // EDSR
    case 0x12: return static_cast<uint16>(m_vdp.previousCommandAddress >> 3);                 // LOPR
    case 0x14: return static_cast<uint16>(m_vdp.commandAddress >> 3);                         // COPR
    case 0x16: return static_cast<uint16>(0x1000 | ((m_vdp.FBCR & 0x1E) << 3) | (m_vdp.TVMR & 0xF)); // MODR
    default: return 0;
    }
}

void Machine::Vdp1WriteRegister(uint32 address, uint16 value) {
    auto &v = m_vdp;
    switch (address) {
    case 0x00:
        v.TVMR = value;
        v.vblankErase = value & 8;
        break;
    case 0x02:
        v.FBCR = value;
        v.swapTrigger = value & 1;
        v.swapMode = value & 2;
        v.paramsChanged = true;
        break;
    case 0x04:
        v.PTMR = value & 3;
        if (v.PTMR == 1) {
            Vdp1Draw(); // (draw now)
        }
        break;
    case 0x06: v.EWDR = value; break;
    case 0x08: v.EWLR = value; break;
    case 0x0A: v.EWRR = value; break;
    default: break;
    }
    m_video->WriteVdp1Reg(address, value);
}

void Machine::Vdp1Swap() {
    auto &v = m_vdp;
    v.previousCommandAddress = v.commandAddress;
    v.previousFrameEnded = v.frameEnded;
    v.frameEnded = false;
    m_video->SwapFramebuffer();
    if (v.PTMR & 2) {
        Vdp1Draw(); // (drawing starts with every swap)
    }
}

// The VDP1 goes through its command table, here all at once: each entry is a command (drawn by the renderer,
// unless marked to be skipped) and says where the next one is; the table's end is marked in a command word.
// The game is told a little later that the drawing is done.
void Machine::Vdp1Draw() {
    auto &v = m_vdp;
    v.frameEnded = false;
    m_video->BeginDrawing();
    constexpr uint32 kNoReturn = 0xFFFFFFFF;
    uint32 address = 0, returnAddress = kNoReturn;
    uint32 commands = 0;
    bool ended = false;
    for (; commands < 0x4000; commands++) {
        const uint16 control = Get16(&m_vram1[address]);
        v.commandAddress = address;
        if (control & 0x8000) {
            ended = true;
            break;
        }
        if (!(control & 0x4000) && (control & 0x000F) <= 0x000B) {
            m_video->DrawCommand(address);
        }
        const uint32 link = (static_cast<uint32>(Get16(&m_vram1[address + 2])) << 3) & ~0x1Fu;
        uint32 target = address;
        switch ((control >> 12) & 3) {
        case 0: target += 0x20; break;
        case 1: target = link; break;
        case 2:
            if (returnAddress == kNoReturn) {
                returnAddress = address + 0x20;
            }
            target = link;
            break;
        default:
            if (returnAddress != kNoReturn) {
                target = returnAddress;
                returnAddress = kNoReturn;
            } else {
                target += 0x20;
            }
            break;
        }
        if (target == address) {
            break; // (a command that leads to itself: the table never ends)
        }
        address = target & 0x7FFE0;
    }
    m_video->EndDrawing();
    if (ended) {
        v.drawEndDue = m_now + 2000 + commands * 64;
    }
}

} // namespace satrecomp::machine
