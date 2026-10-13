// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The VDP2's scroll screens, a line at a time: the four normal screens (NBG0-3: cells or a bitmap, scrolled
// and zoomed), the rotation screen RBG0 (a plane seen in perspective, with a coefficient per line or per dot)
// and the windows that cut pixels out of them.
//
// Not done here (such lines are counted and reported): mosaic, vertical cell scroll, RBG1, and the deinterlace
// and transparent mesh enhancements.
#include "renderer.h"

#include <algorithm>
#include <cstdio>

namespace satrecomp::video {

namespace {
    // Sizes of a page of pattern names in bytes, as a shift: [2x2 cell characters][two word names]
    constexpr uint32 kPageBytesShift[2][2] = {{13, 14}, {11, 12}};
} // namespace

SV_INLINE uint32 Renderer::Color(uint32 index) const {
    // Colour RAM holds 1024 or 2048 15-bit colours, or 1024 24-bit ones
    const uint32 mode = m_regs2.vramControl.colorRAMMode;
    if (mode >= 2) {
        return CramLong((index * 4) & 0xFFC);
    }
    return ConvertRGB555to888(Color555{CramWord((index * 2) & (mode == 0 ? 0x7FE : 0xFFE))}).u32;
}

void Renderer::Colors(uint32 *out) const {
    for (uint32 i = 0; i < 2048; i++) {
        out[i] = Color(i) | 0xFF000000;
    }
}

void Renderer::LayersBeginFrame() {
    const Vdp2Regs &regs2 = m_regs2;
    for (uint32 i = 0; i < 4; i++) {
        if (i == 0 && regs2.bgEnabled[5]) {
            continue;
        }
        const BGParams &bg = regs2.bgParams[i + 1];
        Scroll &scroll = m_scroll[i];
        scroll.fracX = 0;
        scroll.fracY = 0;
        // (the odd field of a double density picture shows the lines between the even field's)
        if (regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity && regs2.TVSTAT.ODD) {
            scroll.fracY += bg.scrollIncV;
        }
        if (i < 2) {
            scroll.lineTable = bg.lineScrollTableAddress;
        }
    }
}

// The rotation parameters' "read again" flags are taken at the start of a line
void Renderer::LayersBeforeLine() {
    const bool taken = m_regs2.displayEnabledLatch && (m_regs2.bgEnabled[4] || m_regs2.bgEnabled[5]);
    for (uint32 i = 0; i < 2; i++) {
        RotationParams &params = m_regs2.rotParams[i];
        m_rot[i].readXst = params.readXst;
        m_rot[i].readYst = params.readYst;
        m_rot[i].readKAst = params.readKAst;
        if (taken) {
            params.readXst = params.readYst = params.readKAst = false;
        }
    }
}

void Renderer::LayersPrepareLine(uint32 y) {
    const Vdp2Regs &regs2 = m_regs2;
    if (!regs2.displayEnabledLatch) {
        return;
    }
    if (regs2.bgEnabled[4] || regs2.bgEnabled[5]) {
        RotationParameters(y);
    }

    // The back screen's colour (one for the picture or one per line)
    {
        const LineBackScreenParams &params = regs2.backScreenParams;
        const uint32 address = params.baseAddress + (params.perLine ? y * 2 : 0);
        m_backColor = ConvertRGB555to888(Color555{.u16 = Vram2Word(address)}).u32;
    }

    // Line scroll (NBG0 and NBG1): a table in VRAM with a horizontal position, a vertical position and a
    // horizontal zoom for every line (or every 2, 4 or 8 lines), whichever of the three are enabled
    for (uint32 i = 0; i < 2; i++) {
        const BGParams &bg = regs2.bgParams[i + 1];
        Scroll &scroll = m_scroll[i];
        scroll.incH = bg.scrollIncH;
        if ((y & ((1u << bg.lineScrollInterval) - 1)) != 0) {
            continue;
        }
        uint32 address = scroll.lineTable;
        uint32 count = 1;
        if (regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity && (y > 0 || regs2.TVSTAT.ODD)) {
            count++;
        }
        for (uint32 n = 0; n < count; n++) {
            if (bg.lineScrollXEnable) {
                scroll.fracX = bit::extract<8, 26>(Vram2Long(address));
                address += 4;
            }
            if (bg.lineScrollYEnable) {
                scroll.fracY = bit::extract<8, 26>(Vram2Long(address));
                address += 4;
            }
            if (bg.lineZoomEnable) {
                scroll.incH = bit::extract<8, 18>(Vram2Long(address));
                address += 4;
            }
        }
        scroll.lineTable = address;
    }
    m_scroll[2].incH = regs2.bgParams[3].scrollIncH;
    m_scroll[3].incH = regs2.bgParams[4].scrollIncH;

    for (Fetcher &fetcher : m_fetch) {
        fetcher.lastChar = 0xFFFFFFFF;
        fetcher.lastCellX = 0xFF;
    }
}

void Renderer::LayersFinishLine() {
    const Vdp2Regs &regs2 = m_regs2;
    for (uint32 i = 0; i < 4; i++) {
        const uint32 inc = regs2.bgParams[i + 1].scrollIncV;
        m_scroll[i].fracY += regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity ? inc * 2 : inc;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Windows

// A window set marks, for every pixel of the line, whether the pixel is cut out. Two rectangular windows
// (either may take its left and right edges per line from a table) and the sprite window (a bit of the
// sprite layer's pixels), each used as it is or inverted, combined by AND or OR.
template <typename Set>
void Renderer::WindowLine(uint32 row, const Set &set, std::vector<uint8> &out) {
    const Vdp2Regs &regs2 = m_regs2;
    const uint32 width = m_width;
    if (!std::any_of(set.enabled.begin(), set.enabled.end(), [](bool on) { return on; })) {
        std::fill_n(out.begin(), width, 0);
        return;
    }
    const bool isOr = set.logic != WindowLogic::And;
    std::fill_n(out.begin(), width, !isOr);
    const uint32 doubleV = regs2.TVMD.LSMDn == InterlaceMode::SingleDensity;

    for (uint32 i = 0; i < 2; i++) {
        if (!set.enabled[i]) {
            continue;
        }
        const WindowParams &window = regs2.windowParams[i];
        const bool inverted = set.inverted[i];
        const auto sy = static_cast<sint16>(row);
        const int startY = static_cast<sint16>(window.startY) << doubleV;
        const int endY = static_cast<sint16>(window.endY) << doubleV;
        if (sy < startY || sy > endY) {
            // A line above or below the window is all outside it
            if (isOr == inverted) {
                std::fill_n(out.begin(), width, isOr);
                return;
            }
            continue;
        }
        sint16 startX = window.startX;
        sint16 endX = window.endX;
        if (window.lineWindowTableEnable) {
            const uint32 address = window.lineWindowTableAddress + row * 4;
            startX = Vram2Word(address);
            endX = Vram2Word(address + 2);
        }
        // (the edges are signed: games set ends before the screen and expect an empty or a full line)
        if (startX < 0) {
            startX = 0;
        }
        if (endX < 0) {
            if (startX >= endX) {
                startX = 0x3FF;
            }
            endX = 0;
        }
        if (regs2.TVMD.HRESOn < 2) {
            startX >>= 1;
            endX >>= 1;
        }
        if (inverted != isOr) {
            if (startX < static_cast<int>(width)) {
                endX = std::min<sint16>(endX, width - 1);
                if (startX <= endX) {
                    std::fill(out.begin() + startX, out.begin() + endX + 1, isOr);
                }
            }
        } else {
            startX = std::min<sint16>(startX, width);
            std::fill_n(out.begin(), startX, isOr);
            if (endX < static_cast<int>(width)) {
                std::fill(out.begin() + endX + 1, out.begin() + width, isOr);
            }
        }
    }

    if (set.enabled.size() > 2 && set.enabled[2]) {
        const bool inverted = set.inverted[2];
        for (uint32 x = 0; x < width; x++) {
            const bool in = static_cast<bool>(m_sprite.shadowOrWindow[x]) != inverted;
            out[x] = isOr ? (out[x] | in) : (out[x] & in);
        }
    }
}

// Widescreen: a window line worked out for the hardware's picture, moved to the middle of the wider line; what
// is beyond the picture's edges is as the edge is
void Renderer::WidenWindow(std::vector<uint8> &line) const {
    const uint32 extra = WideDots(), width = m_width;
    if (!extra) {
        return;
    }
    const uint8 left = line[0], right = line[width - 1];
    std::copy_backward(line.begin(), line.begin() + width, line.begin() + width + extra);
    std::fill_n(line.begin(), extra, left);
    std::fill_n(line.begin() + extra + width, extra, right);
}

void Renderer::SpriteWindowLine(uint32 row) {
    WindowLine(row, m_regs2.spriteParams.windowSet, m_spriteWindow);
    WidenWindow(m_spriteWindow);
}

void Renderer::WindowLines(uint32 row) {
    const Vdp2Regs &regs2 = m_regs2;
    for (uint32 i = 0; i < 5; i++) {
        WindowLine(row, regs2.bgParams[i].windowSet, m_window[i]);
        WidenWindow(m_window[i]);
    }
    WindowLine(row, regs2.commonRotParams.windowSet, m_rotWindow);
}

// ---------------------------------------------------------------------------------------------------------
// Pixels of cells and bitmaps

// One dot of character or bitmap data at `base`: its colour and priority (0: no pixel)
SV_INLINE void Renderer::Dot(const BGParams &bg, bool bitmap, uint32 base, uint32 pitch, uint32 dotX, uint32 dotY,
                                uint32 palette, bool specialPriority, uint32 &color, uint8 &priority) {
    const Vdp2Regs &regs2 = m_regs2;
    const auto &vram = m_mem.vram2;
    const uint32 offset = dotX + dotY * pitch;
    // (data is read in 8 byte groups; a VRAM bank not set up for character data in this screen's access
    // cycle reads as zero)
    auto data = [&](uint32 address, uint32 size) -> uint32 {
        const uint32 bank = (address >> 17) & 3;
        if (!bg.charPatAccess[bank]) {
            return 0;
        }
        uint32 group = address & ~7u;
        if (bitmap) {
            group += bg.vramDataOffset[bank];
        }
        const uint8 *at = &vram[group & 0x7FFF8];
        switch (size) {
        case 1: return at[address & 7];
        case 2: return (at[address & 6] << 8) | at[(address & 6) + 1];
        default: return Be32(&at[address & 4]);
        }
    };

    uint32 colorData = 0;
    bool palettePixel = true;
    switch (bg.colorFormat) {
    case ColorFormat::Palette16: {
        const uint32 dot = (data(base + (offset >> 1), 1) >> ((~dotX & 1) * 4)) & 0xF;
        if (bg.enableTransparency && dot == 0) {
            priority = 0;
            return;
        }
        colorData = dot;
        color = Color(bg.cramOffset + (palette | dot));
        break;
    }
    case ColorFormat::Palette256: {
        const uint32 dot = data(base + offset, 1);
        if (bg.enableTransparency && dot == 0) {
            priority = 0;
            return;
        }
        colorData = dot;
        color = Color(bg.cramOffset + ((palette & 0x700) | dot));
        break;
    }
    case ColorFormat::Palette2048: {
        const uint32 dot = data(base + offset * 2, 2);
        if (bg.enableTransparency && (dot & 0x7FF) == 0) {
            priority = 0;
            return;
        }
        colorData = dot;
        color = Color(bg.cramOffset + (dot & 0x7FF));
        break;
    }
    case ColorFormat::RGB555: {
        const uint32 dot = data(base + offset * 2, 2);
        if (bg.enableTransparency && !(dot & 0x8000)) {
            priority = 0;
            return;
        }
        color = ConvertRGB555to888(Color555{.u16 = static_cast<uint16>(dot)}).u32;
        palettePixel = false;
        break;
    }
    default: {
        const uint32 dot = data(base + offset * 4, 4);
        if (bg.enableTransparency && !(dot & 0x80000000)) {
            priority = 0;
            return;
        }
        color = dot;
        palettePixel = false;
        break;
    }
    }

    // The priority's lowest bit can come from the character or from the dot's colour
    priority = bg.priorityNumber;
    if (bg.priorityMode == PriorityMode::PerCharacter) {
        priority = (priority & ~1) | static_cast<uint8>(specialPriority);
    } else if (bg.priorityMode == PriorityMode::PerDot) {
        priority &= ~1;
        if (palettePixel && specialPriority) {
            priority |= static_cast<uint8>(regs2.specialFunctionCodes[bg.specialFunctionSelect].colorMatches[(colorData >> 1) & 7]);
        }
    }
}

// A one word pattern name: the character number, palette and flips are packed in 16 bits in one of eight
// ways, with the bits that did not fit taken from a register
SV_INLINE Renderer::Char Renderer::OneWordChar(const BGParams &bg, uint16 word) const {
    const bool fourCell = bg.cellSizeShift != 0;
    const bool large = bg.colorFormat != ColorFormat::Palette16;
    const bool ext = bg.extChar;
    const uint32 numberBits = ext ? 12 : 10;
    const uint32 supplStart = 2 * fourCell + 2 * ext;
    Char ch;
    ch.number = (word & ((1u << numberBits) - 1)) << (2 * fourCell);
    ch.number |= ((bg.supplScrollCharNum & 0x1F) >> supplStart) << (10 + supplStart);
    if (fourCell) {
        ch.number |= bg.supplScrollCharNum & 3;
    }
    ch.number &= 0x7FFF;
    ch.palette = large ? ((word >> 12) & 7) << 4 : (((word >> 12) & 0xF) | bg.supplScrollPalNum);
    ch.palette &= 0x7F;
    ch.specialPriority = bg.supplScrollSpecialPriority;
    ch.flipH = !ext && (word & 0x400);
    ch.flipV = !ext && (word & 0x800);
    return ch;
}

// The pixel of a cell screen at (x, y) of its map. A map is 2x2 planes (4x4 for a rotation screen), a plane
// 1 to 4 pages, a page 64x64 cells named by one or two words each, a character 1 or 2x2 cells of 8x8 dots.
SV_INLINE void Renderer::MapPixel(const BGParams &bg, bool rot, std::span<const uint32> planes, uint32 pageShiftH,
                                     uint32 pageShiftV, uint32 x, uint32 y, Fetcher &fetcher, uint32 &color, uint8 &priority) {
    const uint32 planeShift = rot ? 2 : 1;
    const uint32 planeMask = (1u << planeShift) - 1;
    const uint32 fourCell = bg.cellSizeShift != 0;
    const uint32 plane = ((x >> (9 + pageShiftH)) & planeMask) + (((y >> (9 + pageShiftV)) & planeMask) << planeShift);
    const uint32 planeAddress = planes[plane];
    const uint32 bank = (planeAddress >> 17) & 3;
    x += bg.vramDataOffset[bank];

    const uint32 page = (((x >> 9) & 1) & pageShiftH) + ((((y >> 9) & 1) & pageShiftV) << 1);
    const uint32 charIndex = (((x >> 3) & 63) >> fourCell) + ((((y >> 3) & 63) >> fourCell) << (6 - fourCell));
    const uint32 cellX = (x >> 3) & fourCell;
    uint32 cell = cellX + (((y >> 3) & fourCell) << 1);
    uint32 dotX = x & 7, dotY = y & 7;

    if (fetcher.lastChar != charIndex) {
        fetcher.lastChar = charIndex;
        const uint32 pageAddress = planeAddress + (page << kPageBytesShift[fourCell][bg.twoWordChar]);
        Char ch{};
        if (bg.twoWordChar) {
            const uint32 address = pageAddress + charIndex * 4;
            if (bg.patNameAccess[(address >> 17) & 3]) {
                const uint32 name = Vram2Long(address);
                ch.number = name & 0x7FFF;
                ch.palette = (name >> 16) & 0x7F;
                ch.specialPriority = (name >> 29) & 1;
                ch.flipH = (name >> 30) & 1;
                ch.flipV = (name >> 31) & 1;
            }
        } else {
            const uint32 address = pageAddress + charIndex * 2;
            if (bg.patNameAccess[(address >> 17) & 3]) {
                ch = OneWordChar(bg, Vram2Word(address));
            }
        }
        // (a pattern name read too late in the access cycle shows one character late)
        fetcher.current = bg.charPatDelay[bank] ? fetcher.next : ch;
        fetcher.next = ch;
    } else if (fourCell && bg.charPatDelay[bank] && fetcher.lastCellX != cellX) {
        fetcher.lastCellX = cellX;
        if (cellX == 1) {
            fetcher.current = fetcher.next;
        }
    }
    CellPixel(bg, fetcher.current, dotX, dotY, cell, color, priority);
}

SV_INLINE void Renderer::CellPixel(const BGParams &bg, const Char &ch, uint32 dotX, uint32 dotY, uint32 cell,
                                      uint32 &color, uint8 &priority) {
    if (ch.flipH) {
        dotX ^= 7;
        cell ^= bg.cellSizeShift ? 1 : 0;
    }
    if (ch.flipV) {
        dotY ^= 7;
        cell ^= bg.cellSizeShift ? 2 : 0;
    }
    // (a cell is 32 bytes with 16 colours; cells with more colours take 2, 4 or 8 character numbers)
    switch (bg.colorFormat) {
    case ColorFormat::Palette16: break;
    case ColorFormat::Palette256:
    case ColorFormat::Palette2048: cell <<= 1; break;
    case ColorFormat::RGB555: cell <<= 2; break;
    default: cell <<= 3; break;
    }
    Dot(bg, false, (ch.number + cell) << 5, 8, dotX, dotY, static_cast<uint32>(ch.palette) << 4, ch.specialPriority, color,
        priority);
}

// ---------------------------------------------------------------------------------------------------------
// Normal screens

bool Renderer::NormalLine(uint32 index, sint32 offsetY) {
    const Vdp2Regs &regs2 = m_regs2;
    const BGParams &bg = regs2.bgParams[index + 1];
    LayerLine &out = m_layer[index + 2];
    const uint32 width = LineWidth(); // (widescreen: the line starts that much further left in the map)
    if (bg.mosaicEnable || (index < 2 && bg.vcellScrollEnable)) { // (not done here)
        if (m_check) {
            return false;
        }
        m_approximateLines[index + 2]++;
    }
    const Scroll &scroll = m_scroll[index];
    const std::vector<uint8> &window = m_window[index + 1];
    Fetcher &fetcher = m_fetch[index];
    uint32 fracX = scroll.fracX + bg.scrollAmountH - WideDots() * scroll.incH;
    const uint32 y = (scroll.fracY + bg.scrollAmountV + static_cast<uint32>(offsetY)) >> 8;

    if (bg.bitmap) {
        const uint32 dotY = y & (bg.bitmapSizeV - 1);
        for (uint32 x = 0; x < width; x++, fracX += scroll.incH) {
            if (window[x]) {
                out.priority[x] = 0;
                continue;
            }
            Dot(bg, true, bg.bitmapBaseAddress, bg.bitmapSizeH, (fracX >> 8) & (bg.bitmapSizeH - 1), dotY, bg.supplBitmapPalNum,
                bg.supplBitmapSpecialPriority, out.color[x], out.priority[x]);
        }
        return true;
    }
    // The usual case (16 or 256 colours, nothing odd about when in the VRAM access cycle things are read):
    // the pattern name and the row of dots are looked up once per cell instead of for every dot
    const bool plain = bg.colorFormat == ColorFormat::Palette16 || bg.colorFormat == ColorFormat::Palette256;
    bool quirks = false;
    for (uint32 bank = 0; bank < 4; bank++) {
        quirks = quirks || bg.charPatDelay[bank] || bg.vramDataOffset[bank] != 0;
    }
    if (plain && !quirks) {
        static const uint8 kNoDots[8] = {};
        const auto &vram = m_mem.vram2;
        const uint32 fourCell = bg.cellSizeShift != 0;
        const bool wide = bg.colorFormat == ColorFormat::Palette256;
        const auto &matches = regs2.specialFunctionCodes[bg.specialFunctionSelect].colorMatches;
        const uint32 dotY = y & 7;
        const uint32 planeY = ((y >> (9 + bg.pageShiftV)) & 1) << 1, pageY = (((y >> 9) & 1) & bg.pageShiftV) << 1;
        const uint32 charY = (((y >> 3) & 63) >> fourCell) << (6 - fourCell), cellY = ((y >> 3) & fourCell) << 1;
        const uint32 pageBytesShift = kPageBytesShift[fourCell][bg.twoWordChar];
        uint32 current = 0xFFFFFFFF; // the cell the values below are of
        Char ch{};
        const uint8 *dots = kNoDots;
        uint32 palette = 0;
        uint8 priority = 0;
        bool perDot = false;
        for (uint32 x = 0; x < width; x++, fracX += scroll.incH) {
            const uint32 sx = fracX >> 8;
            if ((sx >> 3) != current) {
                current = sx >> 3;
                const uint32 plane = ((sx >> (9 + bg.pageShiftH)) & 1) + planeY;
                const uint32 page = (((sx >> 9) & 1) & bg.pageShiftH) + pageY;
                const uint32 charIndex = (((sx >> 3) & 63) >> fourCell) + charY;
                const uint32 pageAddress = bg.pageBaseAddresses[plane] + (page << pageBytesShift);
                ch = Char{};
                if (bg.twoWordChar) {
                    const uint32 address = pageAddress + charIndex * 4;
                    if (bg.patNameAccess[(address >> 17) & 3]) {
                        const uint32 name = Vram2Long(address);
                        ch.number = name & 0x7FFF;
                        ch.palette = (name >> 16) & 0x7F;
                        ch.specialPriority = (name >> 29) & 1;
                        ch.flipH = (name >> 30) & 1;
                        ch.flipV = (name >> 31) & 1;
                    }
                } else {
                    const uint32 address = pageAddress + charIndex * 2;
                    if (bg.patNameAccess[(address >> 17) & 3]) {
                        ch = OneWordChar(bg, Vram2Word(address));
                    }
                }
                uint32 cell = ((sx >> 3) & fourCell) + cellY;
                if (ch.flipH) {
                    cell ^= fourCell;
                }
                if (ch.flipV) {
                    cell ^= fourCell << 1;
                }
                const uint32 row = ((ch.number + (wide ? cell << 1 : cell)) << 5) + (ch.flipV ? dotY ^ 7 : dotY) * (wide ? 8 : 4);
                dots = bg.charPatAccess[(row >> 17) & 3] ? &vram[row & 0x7FFFF] : kNoDots;
                palette = static_cast<uint32>(ch.palette) << 4;
                priority = bg.priorityNumber;
                if (bg.priorityMode == PriorityMode::PerCharacter) {
                    priority = (priority & ~1) | static_cast<uint8>(ch.specialPriority);
                } else if (bg.priorityMode == PriorityMode::PerDot) {
                    priority &= ~1;
                }
                perDot = bg.priorityMode == PriorityMode::PerDot && ch.specialPriority;
            }
            if (window[x]) {
                out.priority[x] = 0;
                continue;
            }
            const uint32 from = ch.flipH ? (sx & 7) ^ 7 : sx & 7;
            const uint32 dot = wide ? dots[from] : (dots[from >> 1] >> ((~from & 1) * 4)) & 0xF;
            if (dot == 0 && bg.enableTransparency) {
                out.priority[x] = 0;
                continue;
            }
            out.color[x] = Color(bg.cramOffset + (wide ? ((palette & 0x700) | dot) : (palette | dot)));
            out.priority[x] = perDot ? static_cast<uint8>(priority | static_cast<uint8>(matches[(dot >> 1) & 7])) : priority;
        }
        // (what the dot by dot way leaves behind, should the next line be drawn that way)
        fetcher.current = fetcher.next = ch;
        return true;
    }
    for (uint32 x = 0; x < width; x++, fracX += scroll.incH) {
        if (window[x]) {
            out.priority[x] = 0;
            continue;
        }
        MapPixel(bg, false, bg.pageBaseAddresses, bg.pageShiftH, bg.pageShiftV, fracX >> 8, y, fetcher, out.color[x],
                 out.priority[x]);
    }
    // (the hardware reads one more character past the right edge; it matters to the delayed pattern names)
    uint32 color;
    uint8 priority;
    MapPixel(bg, false, bg.pageBaseAddresses, bg.pageShiftH, bg.pageShiftV, fracX >> 8, y, fetcher, color, priority);
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Rotation screen

// A coefficient of the table at `address` (16.10 fixed point index): a scale or a viewpoint position, a
// transparent flag and, in two word tables, line colour data
bool Renderer::Coefficient(const RotationParams &params, uint32 address, bool always, sint32 &value,
                           bool &transparent) const {
    const Vdp2Regs &regs2 = m_regs2;
    const uint32 offset = address >> 10;
    const bool fromCRAM = regs2.vramControl.colorRAMCoeffTableEnable;
    if (!fromCRAM && !always) {
        // (only from a VRAM bank set up for coefficients)
        const uint32 byteAddress = (offset * 4) >> params.coeffDataSize;
        if (!m_regs2.coeffAccess[(byteAddress >> 17) & 3]) {
            return false;
        }
    }
    const bool viewpoint = params.coeffDataMode == CoefficientDataMode::ViewpointX;
    if (params.coeffDataSize == 1) {
        const uint16 data = fromCRAM ? CramWord((offset * 2) | 0x800) : Vram2Word(offset * 2);
        value = bit::extract_signed<0, 14>(data);
        value <<= viewpoint ? 14 : 6;
        transparent = data & 0x8000;
    } else {
        const uint32 data = fromCRAM ? CramLong((offset * 4) | 0x800) : Vram2Long(offset * 4);
        value = bit::extract_signed<0, 23>(data);
        if (viewpoint) {
            value <<= 8;
        }
        transparent = data & 0x80000000;
    }
    return true;
}

// For each of the two rotation parameter sets, where in the plane every dot of this line looks. The game
// gives a table per set: a start point that moves down the screen by a step per line, a step per dot, a 2x3
// matrix, the viewpoint and centre, and scales; a coefficient table can replace a scale or the viewpoint per
// line or per dot, which is what gives a floor its perspective.
void Renderer::RotationParameters(uint32 y) {
    const Vdp2Regs &regs2 = m_regs2;
    const uint32 baseAddress = regs2.commonRotParams.baseAddress & 0xFFF7C;
    const uint32 count = m_width > 352 ? m_width >> 1 : m_width;
    const bool perDot = regs2.vramControl.perDotRotationCoeffs;
    // (widescreen: the line goes on at the same step beyond both ends; a coefficient for each dot stays as it
    // is at the end)
    const uint32 extra = RotationWideDots();

    for (uint32 i = 0; i < 2; i++) {
        const RotationParams &params = regs2.rotParams[i];
        Rotation &rot = m_rot[i];
        if (rot.x.size() < count + 2 * extra) {
            rot.x.resize(count + 2 * extra);
            rot.y.resize(count + 2 * extra);
            rot.transparent.resize(count + 2 * extra);
        }
        RotationParamTable t{};
        t.ReadFrom(&m_mem.vram2[(baseAddress + i * 0x80) & 0x7FFFF]);

        // (the start values are read on the first line and whenever the game has written them since; on
        // other lines they move on by their steps)
        if (y == 0 || rot.readXst) {
            rot.Xst = t.Xst;
        } else {
            rot.Xst += t.deltaXst;
        }
        if (y == 0 || rot.readYst) {
            rot.Yst = t.Yst;
        } else {
            rot.Yst += t.deltaYst;
        }
        if (y == 0 || rot.readKAst) {
            rot.KA = params.coeffTableAddressOffset + t.KAst;
        } else {
            rot.KA += t.dKAst;
        }
        rot.readXst = rot.readYst = rot.readKAst = false;

        const sint32 Xsp = (static_cast<sint64>(t.A) * (rot.Xst - (t.Px << 10)) + static_cast<sint64>(t.B) * (rot.Yst - (t.Py << 10)) +
                            static_cast<sint64>(t.C) * (t.Zst - (t.Pz << 10))) >> 10;
        const sint32 Ysp = (static_cast<sint64>(t.D) * (rot.Xst - (t.Px << 10)) + static_cast<sint64>(t.E) * (rot.Yst - (t.Py << 10)) +
                            static_cast<sint64>(t.F) * (t.Zst - (t.Pz << 10))) >> 10;
        sint32 Xp = (t.A * (t.Px - t.Cx) + t.B * (t.Py - t.Cy) + t.C * (t.Pz - t.Cz)) + (t.Cx << 10) + t.Mx;
        const sint32 Yp = (t.D * (t.Px - t.Cx) + t.E * (t.Py - t.Cy) + t.F * (t.Pz - t.Cz)) + (t.Cy << 10) + t.My;
        const sint32 incX = (t.A * t.deltaX + t.B * t.deltaY) >> 10;
        const sint32 incY = (t.D * t.deltaX + t.E * t.deltaY) >> 10;
        sint64 kx = t.kx, ky = t.ky;
        sint32 scrX = Xsp, scrY = Ysp;
        uint32 KA = rot.KA;

        sint32 value = 0;
        bool transparent = true;
        Coefficient(params, KA, !perDot, value, transparent);
        auto apply = [&] {
            switch (params.coeffDataMode) {
            case CoefficientDataMode::ScaleCoeffXY: kx = ky = value; break;
            case CoefficientDataMode::ScaleCoeffX: kx = value; break;
            case CoefficientDataMode::ScaleCoeffY: ky = value; break;
            case CoefficientDataMode::ViewpointX: Xp = value << 2; break;
            }
        };
        const bool table = params.coeffTableEnable;
        if (table && !perDot) {
            apply();
        }
        // (for a GPU backend: where the line's first and last dots look, with their fractions; between them
        // the positions lie on a straight line unless a coefficient changes from dot to dot)
        rot.straight = !(table && perDot);
        rot.from[0] = static_cast<double>(((kx * scrX) >> 16) + Xp) / 1024.0;
        rot.from[1] = static_cast<double>(((ky * scrY) >> 16) + Yp) / 1024.0;
        rot.to[0] = static_cast<double>(((kx * (scrX + static_cast<sint64>(incX) * (count - 1))) >> 16) + Xp) / 1024.0;
        rot.to[1] = static_cast<double>(((ky * (scrY + static_cast<sint64>(incY) * (count - 1))) >> 16) + Yp) / 1024.0;
        scrX -= static_cast<sint32>(extra) * incX;
        scrY -= static_cast<sint32>(extra) * incY;
        for (uint32 x = 0; x < count + 2 * extra; x++) {
            if (table && perDot) {
                apply();
            }
            rot.transparent[x] = table && transparent;
            rot.x[x] = static_cast<sint32>((((kx * scrX) >> 16) + Xp) >> 10);
            rot.y[x] = static_cast<sint32>((((ky * scrY) >> 16) + Yp) >> 10);
            scrX += incX;
            scrY += incY;
            if (table && perDot && x >= extra && x < extra + count) {
                KA += t.dKAx;
                Coefficient(params, KA, false, value, transparent);
            }
        }
    }
}

bool Renderer::RotationLine() {
    const Vdp2Regs &regs2 = m_regs2;
    const Vdp2Regs &state2 = m_regs2;
    const BGParams &bg = regs2.bgParams[0];
    LayerLine &out = m_layer[1];
    if (bg.mosaicEnable || bg.lineColorScreenEnable) { // (not done here)
        if (m_check) {
            return false;
        }
        m_approximateLines[1]++;
    }
    const bool wide = m_width > 352;
    const uint32 shift = wide ? 1 : 0;
    // (widescreen: so many more dots at both ends, RotationParameters)
    const uint32 extra = RotationWideDots(), count = (m_width >> shift) + 2 * extra;
    const std::vector<uint8> &window = m_window[0];

    for (uint32 x = 0; x < count; x++) {
        const uint32 xx = x << shift;
        // Which of the two parameter sets this dot uses
        uint32 select = 0;
        switch (regs2.commonRotParams.rotParamMode) {
        case RotationParamMode::RotationParamA: select = 0; break;
        case RotationParamMode::RotationParamB: select = 1; break;
        case RotationParamMode::Coefficient: select = regs2.rotParams[0].coeffTableEnable && m_rot[0].transparent[x]; break;
        case RotationParamMode::Window: select = m_rotWindow[std::clamp<sint32>(static_cast<sint32>(x) - static_cast<sint32>(extra), 0, static_cast<sint32>(count - 2 * extra) - 1)]; break;
        }
        const RotationParams &params = regs2.rotParams[select];
        const Rotation &rot = m_rot[select];
        uint32 color = 0;
        uint8 priority = 0;
        bool drawn = false;
        if (params.coeffTableEnable && rot.transparent[x]) {
            // (the coefficient says: nothing here)
        } else if (window[xx] && (!wide || window[xx + 1])) {
        } else {
            const uint32 sx = rot.x[x], sy = rot.y[x];
            const bool fixed512 = params.screenOverProcess == ScreenOverProcess::Fixed512;
            if (bg.bitmap) {
                const uint32 maxX = fixed512 ? 512 : bg.bitmapSizeH, maxY = fixed512 ? 512 : bg.bitmapSizeV;
                if ((sx < maxX && sy < maxY) || params.screenOverProcess == ScreenOverProcess::Repeat) {
                    Dot(bg, true, params.bitmapBaseAddress, bg.bitmapSizeH, sx & (bg.bitmapSizeH - 1), sy & (bg.bitmapSizeV - 1),
                        bg.supplBitmapPalNum, bg.supplBitmapSpecialPriority, color, priority);
                    drawn = true;
                }
            } else {
                const uint32 maxX = fixed512 ? 512 : ((512 * 4) << params.pageShiftH);
                const uint32 maxY = fixed512 ? 512 : ((512 * 4) << params.pageShiftV);
                if ((sx < maxX && sy < maxY) || params.screenOverProcess == ScreenOverProcess::Repeat) {
                    MapPixel(bg, true, state2.rbgPageBaseAddresses[select][0], params.pageShiftH, params.pageShiftV, sx, sy,
                             m_fetch[4 + select], color, priority);
                    drawn = true;
                } else if (params.screenOverProcess == ScreenOverProcess::RepeatChar) {
                    // Outside the map: one character, repeated
                    m_fetch[4].current = OneWordChar(bg, params.screenOverPatternName);
                    CellPixel(bg, m_fetch[4].current, sx & 7, sy & 7, 0, color, priority);
                    drawn = true;
                }
            }
        }
        if (!drawn) {
            out.priority[xx] = 0;
            if (wide) {
                out.priority[xx + 1] = 0;
            }
            continue;
        }
        // (in a high resolution picture each dot of the rotation screen is two pixels wide; the window can
        // cut out either. A pixel left alone keeps what the line before had there.)
        if (!wide || !window[xx]) {
            out.color[xx] = color;
            out.priority[xx] = priority;
        }
        if (wide && !window[xx + 1]) {
            out.color[xx + 1] = color;
            out.priority[xx + 1] = priority;
        }
    }
    return true;
}

// For a GPU backend that draws the rotation screen itself, at its own resolution: this line as where its
// first and last dots look in the map (out[0..3]) and whether anything shows (out[4]: 1, or 0 where the
// coefficient table says nothing does). False if the line is not one a backend can draw from that: it is
// then drawn here.
bool Renderer::FloorLine(float out[8]) {
    const Vdp2Regs &regs2 = m_regs2;
    const BGParams &bg = regs2.bgParams[0];
    const auto &common = regs2.commonRotParams;
    auto none = [](const auto &set) { return !std::any_of(set.enabled.begin(), set.enabled.end(), [](bool on) { return on; }); };
    const bool setA = common.rotParamMode == RotationParamMode::RotationParamA ||
                      (common.rotParamMode == RotationParamMode::Window && none(common.windowSet));
    const RotationParams &params = regs2.rotParams[0];
    const Rotation &rot = m_rot[0];
    bool plain = setA && rot.straight && !regs2.bgEnabled[5] && !bg.bitmap && !bg.mosaicEnable && !bg.lineColorScreenEnable &&
                 none(bg.windowSet) && (bg.colorFormat == ColorFormat::Palette16 || bg.colorFormat == ColorFormat::Palette256) &&
                 bg.priorityMode != PriorityMode::PerDot && params.screenOverProcess != ScreenOverProcess::RepeatChar;
    for (uint32 bank = 0; bank < 4; bank++) {
        plain = plain && !bg.charPatDelay[bank] && bg.vramDataOffset[bank] == 0;
    }
    if (!plain) {
        return false;
    }
    // (widescreen: the line goes on at the same step beyond both ends)
    const uint32 dots = m_width > 352 ? m_width >> 1 : m_width;
    const double more = m_gpu && dots > 1 ? static_cast<double>(m_wideExtra) / (dots - 1) : 0.0;
    const double stepX = (rot.to[0] - rot.from[0]) * more, stepY = (rot.to[1] - rot.from[1]) * more;
    out[0] = static_cast<float>(rot.from[0] - stepX);
    out[1] = static_cast<float>(rot.from[1] - stepY);
    out[2] = static_cast<float>(rot.to[0] + stepX);
    out[3] = static_cast<float>(rot.to[1] + stepY);
    out[4] = params.coeffTableEnable && rot.transparent[RotationWideDots()] ? 0.0f : 1.0f;
    out[5] = out[6] = out[7] = 0;
    return true;
}

// The rotation screen's settings as a GPU backend needs them to read the map itself
void Renderer::FloorSettings(Frame &frame) const {
    const Vdp2Regs &regs2 = m_regs2;
    const BGParams &bg = regs2.bgParams[0];
    const RotationParams &params = regs2.rotParams[0];
    Frame::Floor &floor = frame.floor;
    floor.fourCell = bg.cellSizeShift != 0;
    floor.twoWord = bg.twoWordChar;
    floor.extended = bg.extChar;
    floor.colors256 = bg.colorFormat == ColorFormat::Palette256;
    floor.transparency = bg.enableTransparency;
    floor.priorityPerCharacter = bg.priorityMode == PriorityMode::PerCharacter;
    floor.over = params.screenOverProcess == ScreenOverProcess::Repeat ? 0 : params.screenOverProcess == ScreenOverProcess::Fixed512 ? 2 : 1;
    floor.pageShiftH = static_cast<uint8_t>(params.pageShiftH);
    floor.pageShiftV = static_cast<uint8_t>(params.pageShiftV);
    floor.priority = bg.priorityNumber;
    floor.colorOffset = bg.cramOffset;
    floor.moreNumber = bg.supplScrollCharNum;
    floor.morePalette = bg.supplScrollPalNum;
    floor.specialPriority = bg.supplScrollSpecialPriority;
    floor.access = 0;
    for (uint32 bank = 0; bank < 4; bank++) {
        floor.access |= (bg.patNameAccess[bank] ? 1u : 0u) << bank;
        floor.access |= (bg.charPatAccess[bank] ? 16u : 0u) << bank;
    }
    for (uint32 i = 0; i < 16; i++) {
        floor.planes[i] = m_regs2.rbgPageBaseAddresses[0][0][i];
    }
}

} // namespace satrecomp::video
