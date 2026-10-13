// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The video chips' registers as the renderer reads them: decoded from the raw register words the game wrote.
//
// VDP2 (scroll screens, composition): Vdp2Regs::Decode makes the settings below from its 0x120 bytes of
// registers. VDP1 (sprites and polygons into a framebuffer): Vdp1Regs.
#pragma once

#include "base.h"

#include <array>

namespace satrecomp::video {

enum class ColorFormat : uint8 { Palette16, Palette256, Palette2048, RGB555, RGB888 };
enum class PriorityMode : uint8 { PerScreen, PerCharacter, PerDot, Invalid };
enum class WindowLogic : uint8 { Or, And };
enum class RotationParamMode : uint8 { RotationParamA, RotationParamB, Coefficient, Window };
enum class CoefficientDataMode : uint8 { ScaleCoeffXY, ScaleCoeffX, ScaleCoeffY, ViewpointX };
enum class ScreenOverProcess : uint8 { Repeat, RepeatChar, Transparent, Fixed512 };
enum class InterlaceMode : uint8 { None, Invalid, SingleDensity, DoubleDensity };

// Which of the two windows and the sprite window cut pixels out of a screen, each as it is or inverted
template <size_t N>
struct WindowSet {
    std::array<bool, N> enabled{}, inverted{};
    WindowLogic logic = WindowLogic::Or;
};

// A scroll screen: [0] RBG0, [1..4] NBG0..NBG3
struct BGParams {
    bool enableTransparency = false; // dot value 0 shows nothing
    bool bitmap = false;
    bool lineColorScreenEnable = false, mosaicEnable = false, colorCalcEnable = false, shadowEnable = false;
    uint8 priorityNumber = 0;
    PriorityMode priorityMode = PriorityMode::PerScreen;
    uint8 specialFunctionSelect = 0;
    uint32 cellSizeShift = 0;               // 1: characters of 2x2 cells
    uint32 pageShiftH = 0, pageShiftV = 0;  // a plane is this many pages wide and high (as shifts)
    uint32 bitmapSizeH = 512, bitmapSizeV = 256;
    uint32 scrollAmountH = 0, scrollAmountV = 0; // 11.8 fixed point
    uint32 scrollIncH = 0x100, scrollIncV = 0x100;
    std::array<uint16, 4> mapIndices{};
    std::array<uint32, 4> pageBaseAddresses{}; // of the map's 2x2 planes
    uint32 bitmapBaseAddress = 0;
    ColorFormat colorFormat = ColorFormat::Palette16;
    uint32 cramOffset = 0;
    uint32 supplScrollCharNum = 0, supplScrollPalNum = 0, supplBitmapPalNum = 0;
    bool supplScrollSpecialPriority = false, supplBitmapSpecialPriority = false;
    bool extChar = false, twoWordChar = false;
    // What the VRAM access cycle allows this screen in each of the four VRAM banks
    std::array<bool, 4> patNameAccess{}, charPatAccess{}, charPatDelay{};
    std::array<uint32, 4> vramDataOffset{};
    bool vcellScrollEnable = false, lineScrollXEnable = false, lineScrollYEnable = false, lineZoomEnable = false;
    uint8 lineScrollInterval = 0;
    uint32 lineScrollTableAddress = 0;
    WindowSet<3> windowSet;
    uint16 plsz = 0, bmsz = 0;
};

struct RotationParams {
    bool readXst = false, readYst = false, readKAst = false; // the game asked for these to be read again
    bool coeffTableEnable = false;
    uint8 coeffDataSize = 0; // 1: one word a coefficient
    CoefficientDataMode coeffDataMode = CoefficientDataMode::ScaleCoeffXY;
    bool coeffUseLineColorData = false;
    uint32 coeffTableAddressOffset = 0;
    ScreenOverProcess screenOverProcess = ScreenOverProcess::Repeat;
    uint16 screenOverPatternName = 0;
    uint32 pageShiftH = 0, pageShiftV = 0;
    std::array<uint16, 16> mapIndices{};
    uint32 bitmapBaseAddress = 0;
    uint16 plsz = 0;
};

struct CommonRotationParams {
    uint32 baseAddress = 0;
    RotationParamMode rotParamMode = RotationParamMode::RotationParamA;
    WindowSet<2> windowSet;
};

struct WindowParams {
    uint16 startX = 0, startY = 0, endX = 0, endY = 0;
    bool lineWindowTableEnable = false;
    uint32 lineWindowTableAddress = 0;
};

struct SpriteParams {
    uint8 type = 0;
    bool useSpriteWindow = false, mixedFormat = false, colorCalcEnable = false;
    uint32 colorDataOffset = 0;
    std::array<uint8, 8> priorities{};
    bool spriteWindowEnabled = false, spriteWindowInverted = false;
    WindowSet<2> windowSet;
};

struct LineBackScreenParams {
    uint32 baseAddress = 0;
    bool perLine = false, colorCalcEnable = false, shadowEnable = false;
};

struct ColorOffset {
    uint16 r = 0, g = 0, b = 0; // 9 bits, signed
    bool nonZero = false;
};

struct Vdp2Regs {
    struct {
        bool DISP = false, BDCLMD = false;
        InterlaceMode LSMDn = InterlaceMode::None;
        uint8 VRESOn = 0, HRESOn = 0;
        bool IsInterlaced() const {
            return LSMDn == InterlaceMode::SingleDensity || LSMDn == InterlaceMode::DoubleDensity;
        }
    } TVMD;
    struct {
        bool ODD = false; // the field being shown (set by the machine)
    } TVSTAT;
    struct {
        uint8 colorRAMMode = 0;
        bool colorRAMCoeffTableEnable = false, perDotRotationCoeffs = false;
        bool partitionVRAMA = false, partitionVRAMB = false;
        std::array<uint8, 4> rotDataBankSel{}; // per bank: 0 not for rotation, 1 coefficients, 2 pattern names, 3 characters
    } vramControl;
    bool displayEnabledLatch = false, borderColorModeLatch = false; // TVMD's bits as they were when the frame began
    std::array<bool, 6> bgEnabled{}; // NBG0..3, RBG0, RBG1
    std::array<BGParams, 5> bgParams;
    std::array<RotationParams, 2> rotParams;
    CommonRotationParams commonRotParams;
    std::array<WindowParams, 2> windowParams;
    SpriteParams spriteParams;
    LineBackScreenParams lineScreenParams, backScreenParams;
    std::array<bool, 7> colorOffsetEnable{}, colorOffsetSelect{}; // sprites, RBG0, NBG0..3, back
    std::array<ColorOffset, 2> colorOffset;
    struct {
        std::array<bool, 8> colorMatches{};
    } specialFunctionCodes[2];
    uint32 mosaicH = 1, mosaicV = 1;
    uint32 vcellScrollTableAddress = 0;

    // Worked out from the above
    std::array<bool, 6> layerEnabled{}; // sprites, RBG0, NBG0 (or RBG1), NBG1, NBG2, NBG3
    uint32 rbgPageBaseAddresses[2][2][16] = {}; // [parameter set][RBG0, RBG1][plane]
    std::array<bool, 4> coeffAccess{};  // which VRAM banks hold rotation coefficients

    // Makes all of the above (but the latches, the field and the rotation "read again" flags) from the raw
    // registers: `raw[n]` is the register at offset n * 2
    void Decode(const uint16 *raw);

private:
    void AccessCycle(const uint16 *raw);
};

struct Vdp1Regs {
    bool vblankErase = false, hdtvEnable = false, fbRotEnable = false, pixel8Bits = false;
    bool fbSwapTrigger = false, fbSwapMode = false, dblInterlaceDrawLine = false, dblInterlaceEnable = false, evenOddCoordSelect = false;
    uint32 fbSizeH = 512, fbSizeV = 256, eraseOffsetShift = 9;
    uint16 eraseWriteValue = 0, eraseX1 = 0, eraseY1 = 0, eraseX3 = 0, eraseY3 = 0;
    // (the erase settings as they were at the last framebuffer swap: what the next erase uses)
    uint16 eraseWriteValueLatch = 0, eraseX1Latch = 0, eraseY1Latch = 0, eraseX3Latch = 0, eraseY3Latch = 0;

    void Write(uint32 address, uint16 value);
    void LatchEraseParameters() {
        eraseWriteValueLatch = eraseWriteValue;
        eraseX1Latch = eraseX1;
        eraseY1Latch = eraseY1;
        eraseX3Latch = eraseX3;
        eraseY3Latch = eraseY3;
    }
};

// A rotation parameter table as the game writes it into VRAM
struct RotationParamTable {
    void ReadFrom(const uint8 *input) {
        Xst = Signed(Be32(&input[0x00]) >> 6, 23);
        Yst = Signed(Be32(&input[0x04]) >> 6, 23);
        Zst = Signed(Be32(&input[0x08]) >> 6, 23);
        deltaXst = Signed(Be32(&input[0x0C]) >> 6, 13);
        deltaYst = Signed(Be32(&input[0x10]) >> 6, 13);
        deltaX = Signed(Be32(&input[0x14]) >> 6, 13);
        deltaY = Signed(Be32(&input[0x18]) >> 6, 13);
        A = Signed(Be32(&input[0x1C]) >> 6, 14);
        B = Signed(Be32(&input[0x20]) >> 6, 14);
        C = Signed(Be32(&input[0x24]) >> 6, 14);
        D = Signed(Be32(&input[0x28]) >> 6, 14);
        E = Signed(Be32(&input[0x2C]) >> 6, 14);
        F = Signed(Be32(&input[0x30]) >> 6, 14);
        Px = Signed(Be16(&input[0x34]), 14);
        Py = Signed(Be16(&input[0x36]), 14);
        Pz = Signed(Be16(&input[0x38]), 14);
        Cx = Signed(Be16(&input[0x3C]), 14);
        Cy = Signed(Be16(&input[0x3E]), 14);
        Cz = Signed(Be16(&input[0x40]), 14);
        Mx = Signed(Be32(&input[0x44]) >> 6, 24);
        My = Signed(Be32(&input[0x48]) >> 6, 24);
        kx = Signed(Be32(&input[0x4C]), 24);
        ky = Signed(Be32(&input[0x50]), 24);
        KAst = Be32(&input[0x54]) >> 6;
        dKAst = Signed(Be32(&input[0x58]) >> 6, 20);
        dKAx = Signed(Be32(&input[0x5C]) >> 6, 20);
    }
    sint32 Xst, Yst, Zst;      // where the screen starts in the plane (13.10 fixed point)
    sint32 deltaXst, deltaYst; // its step per line (3.10)
    sint32 deltaX, deltaY;     // ... and per dot (3.10)
    sint32 A, B, C, D, E, F;   // the rotation matrix (4.10)
    sint32 Px, Py, Pz;         // the viewpoint
    sint32 Cx, Cy, Cz;         // the centre
    sint32 Mx, My;             // a shift (14.10)
    sint32 kx, ky;             // scales (8.16)
    uint32 KAst;               // the coefficient table's start (16.10) ...
    sint32 dKAst, dKAx;        // ... and its steps per line and per dot (10.10)
};

// What VDP1 commands leave set for the commands after them
struct Vdp1Context {
    uint16 sysClipH = 512, sysClipV = 256;
    uint16 userClipX0 = 0, userClipY0 = 0, userClipX1 = 512, userClipY1 = 256;
    sint32 localCoordX = 0, localCoordY = 0;
};

} // namespace satrecomp::video
