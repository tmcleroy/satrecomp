// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "regs.h"

#include <algorithm>
#include <bit>

namespace satrecomp::video {

namespace {
    // Register numbers (offset / 2)
    enum : unsigned {
        kTVMD = 0x00 / 2, RAMCTL = 0x0E / 2, CYCA0L = 0x10 / 2, BGON = 0x20 / 2, MZCTL = 0x22 / 2, SFSEL = 0x24 / 2, SFCODE = 0x26 / 2,
        CHCTLA = 0x28 / 2, CHCTLB = 0x2A / 2, BMPNA = 0x2C / 2, BMPNB = 0x2E / 2, PNCN0 = 0x30 / 2, PNCR = 0x38 / 2, PLSZ = 0x3A / 2,
        MPOFN = 0x3C / 2, MPOFR = 0x3E / 2, MPABN0 = 0x40 / 2, MPABRA = 0x50 / 2, MPABRB = 0x60 / 2, SCXIN0 = 0x70 / 2,
        SCXIN1 = 0x80 / 2, SCXN2 = 0x90 / 2, SCYN2 = 0x92 / 2, SCXN3 = 0x94 / 2, SCYN3 = 0x96 / 2, SCRCTL = 0x9A / 2, VCSTAU = 0x9C / 2,
        LSTA0U = 0xA0 / 2, LSTA1U = 0xA4 / 2, LCTAU = 0xA8 / 2, BKTAU = 0xAC / 2, RPMD = 0xB0 / 2, KTCTL = 0xB4 / 2, KTAOF = 0xB6 / 2,
        OVPNRA = 0xB8 / 2, RPTAU = 0xBC / 2, WPSX0 = 0xC0 / 2, WCTLA = 0xD0 / 2, WCTLB = 0xD2 / 2, WCTLC = 0xD4 / 2, WCTLD = 0xD6 / 2,
        LWTA0U = 0xD8 / 2, SPCTL = 0xE0 / 2, SDCTL = 0xE2 / 2, CRAOFA = 0xE4 / 2, CRAOFB = 0xE6 / 2, LNCLEN = 0xE8 / 2, SFPRMD = 0xEA / 2,
        CCCTL = 0xEC / 2, PRISA = 0xF0 / 2, PRINA = 0xF8 / 2, PRINB = 0xFA / 2, PRIR = 0xFC / 2, CLOFEN = 0x110 / 2, CLOFSL = 0x112 / 2,
        COAR = 0x114 / 2,
    };

    // A page of pattern names is 64x64 cells; how many bytes that is, and so which bits of a map register
    // count, depends on the character size and on whether names are one or two words
    constexpr uint32 kMapIndexMasks[2][2][4] = {
        {{0x7F, 0x7E, 0x7E, 0x7C}, {0x3F, 0x3E, 0x3E, 0x3C}},
        {{0x1FF, 0x1FE, 0x1FE, 0x1FC}, {0xFF, 0xFE, 0xFE, 0xFC}},
    };
    constexpr uint32 kPageShift[2][2] = {{13, 14}, {11, 12}};
    uint32 PageAddress(uint32 fourCell, uint32 twoWord, uint32 planeSize, uint32 mapIndex) {
        return (mapIndex & kMapIndexMasks[fourCell][twoWord][planeSize]) << kPageShift[fourCell][twoWord];
    }

    // A window control byte: which windows a screen uses and how
    template <size_t N>
    void Windows(WindowSet<N> &set, uint32 byte) {
        set.inverted[0] = Bit(byte, 0);
        set.enabled[0] = Bit(byte, 1);
        set.inverted[1] = Bit(byte, 2);
        set.enabled[1] = Bit(byte, 3);
        if (N > 2) {
            set.inverted[N - 1] = Bit(byte, 4);
            set.enabled[N - 1] = Bit(byte, 5);
        }
        set.logic = Bit(byte, 7) ? WindowLogic::And : WindowLogic::Or;
    }

    void PatternNames(BGParams &bg, uint32 value) {
        bg.supplScrollCharNum = Bits(value, 0, 4);
        bg.supplScrollPalNum = Bits(value, 5, 7) << 4;
        bg.supplScrollSpecialPriority = Bit(value, 9);
        bg.extChar = Bit(value, 14);
        bg.twoWordChar = !Bit(value, 15);
    }
} // namespace

void Vdp2Regs::Decode(const uint16 *raw) {
    // Display mode
    const uint32 tvmd = raw[kTVMD] & 0x81F7;
    TVMD.DISP = Bit(tvmd, 15);
    TVMD.BDCLMD = Bit(tvmd, 8);
    TVMD.LSMDn = static_cast<InterlaceMode>(Bits(tvmd, 6, 7));
    TVMD.VRESOn = static_cast<uint8>(Bits(tvmd, 4, 5));
    TVMD.HRESOn = static_cast<uint8>(Bits(tvmd, 0, 2));

    // Memory control
    const uint32 ramctl = raw[RAMCTL];
    for (uint32 bank = 0; bank < 4; bank++) {
        vramControl.rotDataBankSel[bank] = static_cast<uint8>(Bits(ramctl, bank * 2, bank * 2 + 1));
    }
    vramControl.partitionVRAMA = Bit(ramctl, 8);
    vramControl.partitionVRAMB = Bit(ramctl, 9);
    vramControl.colorRAMMode = static_cast<uint8>(Bits(ramctl, 12, 13));
    vramControl.colorRAMCoeffTableEnable = Bit(ramctl, 15);
    // (coefficients can be read for every dot only from colour RAM or a VRAM bank set aside for them)
    vramControl.perDotRotationCoeffs = vramControl.colorRAMCoeffTableEnable || vramControl.rotDataBankSel[0] == 1 ||
                                       vramControl.rotDataBankSel[2] == 1 || (vramControl.partitionVRAMA && vramControl.rotDataBankSel[1] == 1) ||
                                       (vramControl.partitionVRAMB && vramControl.rotDataBankSel[3] == 1);

    // Screens on, transparency, mosaic, special function codes. (Screens are [0] RBG0, [1..4] NBG0..3 in
    // bgParams; the registers have NBG0..3 first.)
    const uint32 bgon = raw[BGON];
    for (uint32 i = 0; i < 6; i++) {
        bgEnabled[i] = Bit(bgon, i);
    }
    static constexpr uint32 kScreenOfBit[5] = {1, 2, 3, 4, 0}; // register bit n is for this screen
    for (uint32 n = 0; n < 5; n++) {
        BGParams &bg = bgParams[kScreenOfBit[n]];
        bg.enableTransparency = !Bit(bgon, 8 + n);
        bg.mosaicEnable = Bit(raw[MZCTL], n);
        bg.specialFunctionSelect = static_cast<uint8>(Bit(raw[SFSEL], n));
        bg.shadowEnable = Bit(raw[SDCTL], n);
        bg.lineColorScreenEnable = Bit(raw[LNCLEN], n);
        bg.colorCalcEnable = Bit(raw[CCCTL], n);
        bg.priorityMode = static_cast<PriorityMode>(Bits(raw[SFPRMD], n * 2, n * 2 + 1));
    }
    mosaicH = Bits(raw[MZCTL], 8, 11) + 1;
    mosaicV = Bits(raw[MZCTL], 12, 15) + 1;
    for (uint32 i = 0; i < 8; i++) {
        specialFunctionCodes[0].colorMatches[i] = Bit(raw[SFCODE], i);
        specialFunctionCodes[1].colorMatches[i] = Bit(raw[SFCODE], 8 + i);
    }

    // Character control: cell size, bitmap, colours
    const uint32 chctla = raw[CHCTLA], chctlb = raw[CHCTLB];
    bgParams[1].cellSizeShift = Bit(chctla, 0);
    bgParams[1].bitmap = Bit(chctla, 1);
    bgParams[1].bmsz = static_cast<uint16>(Bits(chctla, 2, 3));
    bgParams[1].colorFormat = static_cast<ColorFormat>(std::min<uint32>(Bits(chctla, 4, 6), 4));
    bgParams[2].cellSizeShift = Bit(chctla, 8);
    bgParams[2].bitmap = Bit(chctla, 9);
    bgParams[2].bmsz = static_cast<uint16>(Bits(chctla, 10, 11));
    bgParams[2].colorFormat = static_cast<ColorFormat>(Bits(chctla, 12, 13));
    bgParams[3].cellSizeShift = Bit(chctlb, 0);
    bgParams[3].colorFormat = static_cast<ColorFormat>(Bit(chctlb, 1));
    bgParams[4].cellSizeShift = Bit(chctlb, 4);
    bgParams[4].colorFormat = static_cast<ColorFormat>(Bit(chctlb, 5));
    bgParams[0].cellSizeShift = Bit(chctlb, 8);
    bgParams[0].bitmap = Bit(chctlb, 9);
    bgParams[0].bmsz = static_cast<uint16>(Bit(chctlb, 10));
    bgParams[0].colorFormat = static_cast<ColorFormat>(std::min<uint32>(Bits(chctlb, 12, 14), 4));
    static constexpr uint32 kBitmapW[4] = {512, 512, 1024, 1024}, kBitmapH[4] = {256, 512, 256, 512};
    for (BGParams &bg : bgParams) {
        bg.bitmapSizeH = kBitmapW[bg.bmsz & 3];
        bg.bitmapSizeV = kBitmapH[bg.bmsz & 3];
    }
    bgParams[1].supplBitmapPalNum = Bits(raw[BMPNA], 0, 2) << 8;
    bgParams[1].supplBitmapSpecialPriority = Bit(raw[BMPNA], 5);
    bgParams[2].supplBitmapPalNum = Bits(raw[BMPNA], 8, 10) << 8;
    bgParams[2].supplBitmapSpecialPriority = Bit(raw[BMPNA], 13);
    bgParams[0].supplBitmapPalNum = Bits(raw[BMPNB], 0, 2) << 8;
    bgParams[0].supplBitmapSpecialPriority = Bit(raw[BMPNB], 5);

    // Pattern names, plane sizes, maps
    for (uint32 n = 0; n < 4; n++) {
        PatternNames(bgParams[n + 1], raw[PNCN0 + n]);
    }
    PatternNames(bgParams[0], raw[PNCR]);
    const uint32 plsz = raw[PLSZ];
    for (uint32 n = 0; n < 4; n++) {
        BGParams &bg = bgParams[n + 1];
        bg.plsz = static_cast<uint16>(Bits(plsz, n * 2, n * 2 + 1));
        bg.pageShiftH = bg.plsz & 1;
        bg.pageShiftV = bg.plsz >> 1;
        const uint32 offset = Bits(raw[MPOFN], n * 4, n * 4 + 2);
        bg.bitmapBaseAddress = offset << 17;
        for (uint32 plane = 0; plane < 4; plane++) {
            const uint32 index = Bits(raw[MPABN0 + n * 2 + plane / 2], (plane & 1) * 8, (plane & 1) * 8 + 5);
            bg.mapIndices[plane] = static_cast<uint16>((offset << 6) | index);
            bg.pageBaseAddresses[plane] = PageAddress(bg.cellSizeShift, bg.twoWordChar, bg.plsz, bg.mapIndices[plane]);
        }
    }
    for (uint32 set = 0; set < 2; set++) {
        RotationParams &rot = rotParams[set];
        rot.plsz = static_cast<uint16>(Bits(plsz, 8 + set * 4, 9 + set * 4));
        rot.screenOverProcess = static_cast<ScreenOverProcess>(Bits(plsz, 10 + set * 4, 11 + set * 4));
        rot.pageShiftH = rot.plsz & 1;
        rot.pageShiftV = rot.plsz >> 1;
        const uint32 offset = Bits(raw[MPOFR], set * 4, set * 4 + 2);
        rot.bitmapBaseAddress = offset << 17;
        for (uint32 plane = 0; plane < 16; plane++) {
            const uint32 index = Bits(raw[(set ? MPABRB : MPABRA) + plane / 2], (plane & 1) * 8, (plane & 1) * 8 + 5);
            rot.mapIndices[plane] = static_cast<uint16>((offset << 6) | index);
        }
        const uint32 ktctl = raw[KTCTL] >> (set * 8);
        rot.coeffTableEnable = Bit(ktctl, 0);
        rot.coeffDataSize = static_cast<uint8>(Bit(ktctl, 1));
        rot.coeffDataMode = static_cast<CoefficientDataMode>(Bits(ktctl, 2, 3));
        rot.coeffUseLineColorData = Bit(ktctl, 4);
        rot.coeffTableAddressOffset = Bits(raw[KTAOF], set * 8, set * 8 + 2) << 26;
        rot.screenOverPatternName = raw[OVPNRA + set];
    }
    for (uint32 screen = 0; screen < 2; screen++) { // RBG0, RBG1
        const BGParams &bg = bgParams[screen];
        for (uint32 set = 0; set < 2; set++) {
            for (uint32 plane = 0; plane < 16; plane++) {
                rbgPageBaseAddresses[set][screen][plane] =
                    PageAddress(bg.cellSizeShift, bg.twoWordChar, rotParams[set].plsz, rotParams[set].mapIndices[plane]);
            }
        }
    }

    // Scroll positions and zoom (11.8 fixed point), line scroll
    for (uint32 n = 0; n < 2; n++) {
        BGParams &bg = bgParams[n + 1];
        const uint16 *r = &raw[n ? SCXIN1 : SCXIN0];
        bg.scrollAmountH = (Bits(r[0], 0, 10) << 8) | Bits(r[1], 8, 15);
        bg.scrollAmountV = (Bits(r[2], 0, 10) << 8) | Bits(r[3], 8, 15);
        bg.scrollIncH = (Bits(r[4], 0, 2) << 8) | Bits(r[5], 8, 15);
        bg.scrollIncV = (Bits(r[6], 0, 2) << 8) | Bits(r[7], 8, 15);
        const uint32 scrctl = raw[SCRCTL] >> (n * 8);
        bg.vcellScrollEnable = Bit(scrctl, 0);
        bg.lineScrollXEnable = Bit(scrctl, 1);
        bg.lineScrollYEnable = Bit(scrctl, 2);
        bg.lineZoomEnable = Bit(scrctl, 3);
        bg.lineScrollInterval = static_cast<uint8>(Bits(scrctl, 4, 5));
        const uint16 *table = &raw[n ? LSTA1U : LSTA0U];
        bg.lineScrollTableAddress = (Bits(table[0], 0, 2) << 17) | (Bits(table[1], 1, 15) << 2);
    }
    bgParams[3].scrollAmountH = Bits(raw[SCXN2], 0, 10) << 8;
    bgParams[3].scrollAmountV = Bits(raw[SCYN2], 0, 10) << 8;
    bgParams[4].scrollAmountH = Bits(raw[SCXN3], 0, 10) << 8;
    bgParams[4].scrollAmountV = Bits(raw[SCYN3], 0, 10) << 8;
    bgParams[3].scrollIncH = bgParams[3].scrollIncV = bgParams[4].scrollIncH = bgParams[4].scrollIncV = 0x100;
    vcellScrollTableAddress = (Bits(raw[VCSTAU], 0, 2) << 17) | (Bits(raw[VCSTAU + 1], 1, 15) << 2);

    // Line colour and back screens, rotation parameters
    lineScreenParams.baseAddress = (Bits(raw[LCTAU], 0, 2) << 17) | (static_cast<uint32>(raw[LCTAU + 1]) << 1);
    lineScreenParams.perLine = Bit(raw[LCTAU], 15);
    lineScreenParams.colorCalcEnable = Bit(raw[CCCTL], 5);
    backScreenParams.baseAddress = (Bits(raw[BKTAU], 0, 2) << 17) | (static_cast<uint32>(raw[BKTAU + 1]) << 1);
    backScreenParams.perLine = Bit(raw[BKTAU], 15);
    backScreenParams.shadowEnable = Bit(raw[SDCTL], 5);
    backScreenParams.colorCalcEnable = false;
    commonRotParams.rotParamMode = static_cast<RotationParamMode>(Bits(raw[RPMD], 0, 1));
    commonRotParams.baseAddress = (Bits(raw[RPTAU], 0, 2) << 17) | (Bits(raw[RPTAU + 1], 1, 15) << 2);

    // Windows
    for (uint32 w = 0; w < 2; w++) {
        const uint16 *r = &raw[WPSX0 + w * 4];
        windowParams[w].startX = r[0];
        windowParams[w].startY = r[1];
        windowParams[w].endX = r[2];
        windowParams[w].endY = r[3];
        windowParams[w].lineWindowTableEnable = Bit(raw[LWTA0U + w * 2], 15);
        windowParams[w].lineWindowTableAddress = (Bits(raw[LWTA0U + w * 2], 0, 2) << 17) | (Bits(raw[LWTA0U + w * 2 + 1], 1, 15) << 2);
    }
    Windows(bgParams[1].windowSet, raw[WCTLA] & 0xFF);
    Windows(bgParams[2].windowSet, raw[WCTLA] >> 8);
    Windows(bgParams[3].windowSet, raw[WCTLB] & 0xFF);
    Windows(bgParams[4].windowSet, raw[WCTLB] >> 8);
    Windows(bgParams[0].windowSet, raw[WCTLC] & 0xFF);
    Windows(spriteParams.windowSet, raw[WCTLC] >> 8);
    spriteParams.spriteWindowInverted = Bit(raw[WCTLC], 12);
    spriteParams.spriteWindowEnabled = Bit(raw[WCTLC], 13);
    Windows(commonRotParams.windowSet, raw[WCTLD] & 0xFF);

    // Sprites, colours, priorities
    spriteParams.type = static_cast<uint8>(Bits(raw[SPCTL], 0, 3));
    spriteParams.useSpriteWindow = Bit(raw[SPCTL], 4);
    spriteParams.mixedFormat = Bit(raw[SPCTL], 5);
    spriteParams.colorCalcEnable = Bit(raw[CCCTL], 6);
    spriteParams.colorDataOffset = Bits(raw[CRAOFB], 4, 6) << 8;
    for (uint32 n = 0; n < 4; n++) {
        bgParams[n + 1].cramOffset = Bits(raw[CRAOFA], n * 4, n * 4 + 2) << 8;
        spriteParams.priorities[n * 2] = static_cast<uint8>(Bits(raw[PRISA + n], 0, 2));
        spriteParams.priorities[n * 2 + 1] = static_cast<uint8>(Bits(raw[PRISA + n], 8, 10));
    }
    bgParams[0].cramOffset = Bits(raw[CRAOFB], 0, 2) << 8;
    bgParams[1].priorityNumber = static_cast<uint8>(Bits(raw[PRINA], 0, 2));
    bgParams[2].priorityNumber = static_cast<uint8>(Bits(raw[PRINA], 8, 10));
    bgParams[3].priorityNumber = static_cast<uint8>(Bits(raw[PRINB], 0, 2));
    bgParams[4].priorityNumber = static_cast<uint8>(Bits(raw[PRINB], 8, 10));
    bgParams[0].priorityNumber = static_cast<uint8>(Bits(raw[PRIR], 0, 2));
    static constexpr uint32 kLayerOfBit[7] = {2, 3, 4, 5, 1, 6, 0}; // NBG0..3, RBG0, back, sprites
    for (uint32 n = 0; n < 7; n++) {
        colorOffsetEnable[kLayerOfBit[n]] = Bit(raw[CLOFEN], n);
        colorOffsetSelect[kLayerOfBit[n]] = Bit(raw[CLOFSL], n);
    }
    for (uint32 set = 0; set < 2; set++) {
        ColorOffset &offset = colorOffset[set];
        offset.r = static_cast<uint16>(Bits(raw[COAR + set * 3], 0, 8));
        offset.g = static_cast<uint16>(Bits(raw[COAR + set * 3 + 1], 0, 8));
        offset.b = static_cast<uint16>(Bits(raw[COAR + set * 3 + 2], 0, 8));
        offset.nonZero = offset.r != 0 || offset.g != 0 || offset.b != 0;
    }

    // Which layers show at all: screens with many colours use up the VRAM access time of others
    if (bgEnabled[4] && bgEnabled[5]) {
        layerEnabled = {true, true, true, false, false, false};
    } else {
        const ColorFormat c0 = bgParams[1].colorFormat, c1 = bgParams[2].colorFormat;
        const bool noNBG1 = c0 == ColorFormat::RGB888;
        const bool noNBG2 = c0 == ColorFormat::Palette2048 || c0 == ColorFormat::RGB555 || c0 == ColorFormat::RGB888;
        const bool noNBG3 = c0 == ColorFormat::RGB888 || c1 == ColorFormat::Palette2048 || c1 == ColorFormat::RGB555;
        layerEnabled = {true, bgEnabled[4], bgEnabled[0] || bgEnabled[5], bgEnabled[1] && !noNBG1, bgEnabled[2] && !noNBG2,
                        bgEnabled[3] && !noNBG3};
    }
    AccessCycle(raw);
}

// The VDP2 reads VRAM in a cycle of eight time slots per dot (four in high resolution), and the game says
// which bank is read for what in each slot. A screen can only fetch pattern names or character data from a
// bank in a slot set up for it; if the character data's slot comes too soon after the pattern name's, the
// screen shows shifted by a character or by eight dots.
void Vdp2Regs::AccessCycle(const uint16 *raw) {
    enum : uint8 { kPatternName0 = 0, kCharacter0 = 4, kCpu = 14 };
    uint8 timings[4][8];
    for (uint32 bank = 0; bank < 4; bank++) {
        for (uint32 slot = 0; slot < 8; slot++) {
            timings[bank][slot] = static_cast<uint8>(Bits(raw[CYCA0L + bank * 2 + slot / 4], 12 - (slot & 3) * 4, 15 - (slot & 3) * 4));
        }
    }
    const bool hires = (TVMD.HRESOn & 6) != 0;
    for (uint32 n = 0; n < 4; n++) {
        bgParams[n + 1].vramDataOffset.fill(0);
    }
    uint8 pn[4] = {}, cp[4] = {};         // per screen: the slots with a pattern name / character access
    uint8 pnBank[4][4] = {}, cpBank[4][4] = {};
    uint8 firstSlot[4] = {0xFF, 0xFF, 0xFF, 0xFF}, firstVram[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    bool found[4][4] = {};
    for (uint32 slot = 0; slot < 8; slot++) {
        for (uint32 bank = 0; bank < 4; bank++) {
            if ((bank == 1 && !vramControl.partitionVRAMA) || (bank == 3 && !vramControl.partitionVRAMB)) {
                continue;
            }
            const uint8 timing = timings[bank][slot];
            if (timing < 4) {
                pn[timing] |= 1u << slot;
                pnBank[timing][bank] |= 1u << slot;
            } else if (timing < 8) {
                const uint32 n = timing - kCharacter0;
                cp[n] |= 1u << slot;
                cpBank[n][bank] |= 1u << slot;
                // (a high resolution bitmap read from both VRAMs: the later one is eight dots behind)
                BGParams &bg = bgParams[n + 1];
                if (bg.bitmap && hires) {
                    const uint8 vram = static_cast<uint8>(bank >> 1);
                    if (firstSlot[n] == 0xFF) {
                        firstSlot[n] = static_cast<uint8>(slot);
                        firstVram[n] = vram;
                    } else if (!found[n][bank] && slot > firstSlot[n] && vram != firstVram[n]) {
                        bg.vramDataOffset[bank] = 8;
                    }
                    found[n][bank] = true;
                }
            }
        }
        if (hires && slot == 3) {
            break;
        }
    }

    const bool rbg0 = bgEnabled[4], rbg1 = bgEnabled[5];
    const uint32 firstNBG = rbg1 ? 1 : 0;
    uint8 needed[4] = {}; // character accesses a screen needs per dot
    for (uint32 n = firstNBG; n < 4; n++) {
        static constexpr uint8 kNeeded[5] = {1, 2, 4, 4, 8};
        needed[n] = kNeeded[static_cast<uint32>(bgParams[n + 1].colorFormat)];
    }
    for (uint32 n = firstNBG; n < 4; n++) {
        BGParams &bg = bgParams[n + 1];
        bg.charPatDelay.fill(false);
        if (bg.bitmap || pn[n] == 0 || cp[n] == 0 || (n < 2 && (pn[n] & 1))) {
            continue;
        }
        const uint32 pnSlot = std::countr_zero(pn[n]);
        if (hires) {
            static constexpr uint8 kInTime[2][4] = {{0b0111, 0b1110, 0b1101, 0b1011}, {0b0111, 0b1110, 0b1100, 0b1000}};
            if (pnSlot < 4 && (cp[n] < pn[n] || (cp[n] & kInTime[bg.cellSizeShift][pnSlot]) == 0)) {
                bg.charPatDelay.fill(true);
            }
        } else if (pnSlot < 8) {
            static constexpr uint8 kInTime[8] = {0b11110111, 0b11101111, 0b11001111, 0b10001111, 0b00001111, 0b00001110, 0b00001100, 0b00001000};
            const bool enough = std::popcount(static_cast<uint8>(cp[n] & kInTime[pnSlot])) >= needed[n];
            for (uint32 bank = 0; bank < 4; bank++) {
                const uint8 cpB = cpBank[n][bank], pnB = pnBank[n][bank];
                if (cpB != 0 && (cpB & kInTime[pnSlot]) == 0 && (!enough || pnB != 0)) {
                    if ((cpB & ~kInTime[pnSlot] & 0xFF) >= 0b10000) {
                        if (bg.cellSizeShift == 0) {
                            bg.charPatDelay.fill(true);
                        }
                    } else if (pnSlot < static_cast<uint32>(std::countr_zero(cp[n]))) {
                        const uint32 pnSlotBank = std::countr_zero(pnB);
                        if (!pnB || (cpB & kInTime[pnSlotBank & 7]) == 0) {
                            bg.vramDataOffset.fill(8);
                        }
                    } else {
                        bg.charPatDelay.fill(true);
                    }
                }
            }
        }
    }

    for (uint32 bank = 0; bank < 4; bank++) {
        // A bank set aside for a rotation screen is that screen's alone
        const uint32 forRotation =
            vramControl.rotDataBankSel[(bank == 1 && !vramControl.partitionVRAMA) ? 0 : (bank == 3 && !vramControl.partitionVRAMB) ? 2 : bank];
        const bool rbg0Here = rbg0 && (!rbg1 || bank < 2);
        bgParams[0].patNameAccess[bank] = rbg0Here && forRotation == 2;
        bgParams[0].charPatAccess[bank] = rbg0Here && forRotation == 3;
        bgParams[1].patNameAccess[bank] = rbg1 && bank == 3;
        bgParams[1].charPatAccess[bank] = rbg1 && bank == 2;
        for (uint32 n = firstNBG; n < 4; n++) {
            BGParams &bg = bgParams[n + 1];
            bg.patNameAccess[bank] = false;
            bg.charPatAccess[bank] = false;
            const uint32 slots = hires ? 4 : 8;
            if (!bgEnabled[n] || (rbg1 && bank >= 2) || (rbg0 && forRotation != 0) || needed[n] > slots ||
                std::popcount(cp[n]) < needed[n]) {
                continue;
            }
            for (uint32 slot = 0; slot < slots; slot++) {
                const uint8 timing = timings[bank][slot];
                if (timing == kPatternName0 + n) {
                    bg.patNameAccess[bank] = true;
                } else if (timing == kCharacter0 + n) {
                    bg.charPatAccess[bank] = true;
                } else if (timing == kCpu && bg.bitmap) {
                    bg.charPatAccess[bank] = true; // (bitmaps show from a bank given to the CPU too)
                }
            }
        }
    }
    // (a VRAM not split in two is one bank)
    for (BGParams &bg : bgParams) {
        if (!vramControl.partitionVRAMA) {
            bg.charPatAccess[1] = bg.charPatAccess[0];
            bg.patNameAccess[1] = bg.patNameAccess[0];
            bg.vramDataOffset[1] = bg.vramDataOffset[0];
        }
        if (!vramControl.partitionVRAMB) {
            bg.charPatAccess[3] = bg.charPatAccess[2];
            bg.patNameAccess[3] = bg.patNameAccess[2];
            bg.vramDataOffset[3] = bg.vramDataOffset[2];
        }
    }
    coeffAccess[0] = vramControl.rotDataBankSel[0] == 1;
    coeffAccess[1] = vramControl.rotDataBankSel[vramControl.partitionVRAMA ? 1 : 0] == 1;
    coeffAccess[2] = vramControl.rotDataBankSel[2] == 1;
    coeffAccess[3] = vramControl.rotDataBankSel[vramControl.partitionVRAMB ? 3 : 2] == 1;
}

void Vdp1Regs::Write(uint32 address, uint16 value) {
    switch (address & 0x1E) {
    case 0x00: { // TVMR: framebuffer format
        vblankErase = Bit(value, 3);
        hdtvEnable = Bit(value, 2);
        fbRotEnable = Bit(value, 1);
        pixel8Bits = Bit(value, 0);
        static constexpr uint32 kSizesH[] = {512, 1024, 512, 512, 512, 512, 512, 512};
        static constexpr uint32 kSizesV[] = {256, 256, 256, 512, 512, 512, 512, 512};
        static constexpr uint32 kEraseShifts[] = {9, 9, 9, 8, 9, 8, 9, 8};
        const uint32 mode = value & 7;
        fbSizeH = kSizesH[mode];
        fbSizeV = kSizesV[mode];
        eraseOffsetShift = kEraseShifts[mode];
        break;
    }
    case 0x02: // FBCR: framebuffer swapping, interlace
        fbSwapTrigger = Bit(value, 0);
        fbSwapMode = Bit(value, 1);
        dblInterlaceDrawLine = Bit(value, 2);
        dblInterlaceEnable = Bit(value, 3);
        evenOddCoordSelect = Bit(value, 4);
        break;
    case 0x06: eraseWriteValue = value; break;
    case 0x08:
        eraseY1 = static_cast<uint16>(Bits(value, 0, 8));
        eraseX1 = static_cast<uint16>(Bits(value, 9, 14) << 3);
        break;
    case 0x0A:
        eraseY3 = static_cast<uint16>(Bits(value, 0, 8));
        eraseX3 = static_cast<uint16>(Bits(value, 9, 15) << 3);
        break;
    default: break;
    }
}

} // namespace satrecomp::video
