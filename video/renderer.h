// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// satrecomp's video renderer: what draws the picture from the video chips' state.
//
// It reads the chips' memory (VDP1 VRAM and framebuffers, VDP2 VRAM and colour RAM) where the machine keeps
// it, is told every register write, and is called at the points of a frame where the hardware does something:
// the VDP1 erasing and swapping its framebuffers and going through its command table, the VDP2 showing a line.
// Nothing here knows which machine model calls it (docs/VIDEO.md).
#pragma once

#include "base.h"
#include "frame.h"
#include "regs.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace satrecomp {
class Archive;
}

namespace satrecomp::video {

// Where the machine keeps the video chips' memory
struct Memory {
    uint8 *vram1 = nullptr; // VDP1 VRAM, 0x80000 bytes
    uint8 *fb[2] = {};      // the VDP1's two framebuffers, 0x40000 bytes each
    uint8 *vram2 = nullptr; // VDP2 VRAM, 0x80000 bytes
    uint8 *cram = nullptr;  // colour RAM, 0x1000 bytes (24-bit colours stored with their halves 0x800 apart)
};

// What a sprite layer pixel is besides its colour
enum class SpriteSpecial : uint8 { Normal, Shadow, Transparent };

// Another renderer drawing the same frames, to compare with (and to take lines from that are not drawn here)
struct Reference {
    virtual ~Reference() = default;
    // Of the line drawn last: a layer's colours and priorities (layer 0 sprites, 1 RBG0, 2..5 NBG0..3) ...
    virtual void LayerLine(uint32 layer, uint32 width, uint32 *color, uint8 *priority) = 0;
    // ... and the sprite layer's other properties (SpriteSpecial, the shadow / window bit)
    virtual void SpriteLine(uint32 width, uint8 *special, uint8 *shadowOrWindow) = 0;
    // Its picture so far
    virtual const uint32 *Picture() = 0;
};

// One VDP1 drawing command as the game wrote it (the 16 words of its table entry) 
struct Command {
    uint32_t address;
    uint16_t words[16]; // CMDCTRL, CMDLINK, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE, XA, YA, ... XD, YD, CMDGRDA, -
};

class Renderer final {
public:
    explicit Renderer(const Memory &memory);

    // How the picture is made:
    //  Reference: every call goes to the hardware model's software renderer.
    //  Software:  everything is drawn here at the original resolution: the VDP1's commands into its
    //             framebuffer (raster.cpp), the scroll screens, the sprite layer, windows, composition.
    //  Check:     both, with every layer of every line and every frame compared; reported at exit.
    // Frames made here come out through `onFrame`.
    //  Gpu:       nothing is rasterised here: the VDP1's commands and the VDP2's layers go to `onGpuFrame` as
    //             a Frame (frame.h) for a GPU backend to draw.
    enum class Mode { Reference, Software, Check, Gpu };
    using FrameCallback = std::function<void(uint32 *pixels, uint32 width, uint32 height)>;
    void SetMode(Mode mode, FrameCallback onFrame, Reference *reference = nullptr);
    void SetGpuFrameCallback(std::function<void(const Frame &)> onGpuFrame) {
        m_onGpuFrame = std::move(onGpuFrame);
    }
    // For tests of a GPU backend: the reference rasterises the VDP1's commands as well, and the backend's
    // framebuffer (at the original resolution) is compared with its; reported at exit
    void SetBothFields(bool on) {
        m_bothFields = on;
    }
    // Widescreen (GPU only): the picture shows this many more VDP1 pixels on each side than the hardware's.
    // The game's 3D view simply continues there; the scroll screens and the floor are drawn further out;
    // scenes without a 3D floor keep the hardware's picture, with black at the sides.
    void SetWidescreen(uint32 extraPerSide) {
        m_wideExtra = extraPerSide;
    }
    // Widescreen: parts of a scroll screen that belong at the picture's sides (a game's gauges, say). In the
    // rows given, what is left of `leftEnd` moves out to the left edge and what is at or right of `rightStart`
    // to the right edge; what is between stays in the middle. (Rows and columns of the hardware's picture.)
    struct WideSplit {
        uint32 layer; // 0..3: NBG0..NBG3
        uint32 rowFrom, rowTo;
        uint32 leftEnd, rightStart;
    };
    void SetWideSplits(std::vector<WideSplit> splits) {
        m_wideSplits = std::move(splits);
    }
    void SetGpuFloor(bool on) {
        m_gpuFloor = on;
    }
    void SetGpuCheck(bool on) {
        m_gpuCheck = on;
    }
    void CompareSprites(const uint16_t *words, uint32_t width, uint32_t height);
    // What the comparison found, on stderr
    void Report() const;

    // The VDP1 commands of the frame drawn last
    const std::vector<Command> &LastCommands() const {
        return m_lastCommands;
    }

    // --- what the machine tells it ---
    void Reset(bool hard);
    void WriteVdp1Reg(uint32 address, uint16 value);
    void WriteVdp2Reg(uint32 address, uint16 value);
    void Vdp1VramWritten() {
        m_vramWritesWhileDrawing += m_drawing;
    }
    // The VDP1: the framebuffer on display erased; the two swapped; the command table gone through
    void EraseFramebuffer(uint64 cycles);
    void SwapFramebuffer();
    void BeginDrawing();
    void DrawCommand(uint32 address);
    void EndDrawing();
    // The VDP2: the picture's size, the field of an interlaced picture, the display mode taking effect at
    // the start of a frame, the frame's lines
    void SetResolution(uint32 h, uint32 v, bool exclusive);
    void SetField(bool odd);
    void LatchDisplayMode();
    void BeginFrame();
    void RenderLine(uint32 y);
    void EndFrame();

    // Save states (runtime/state.h): the registers and what goes from one line or frame to the next
    void Serialize(Archive &ar);

    const Vdp1Regs &Regs1() const {
        return m_regs1;
    }
    const Vdp2Regs &Regs2() const {
        return m_regs2;
    }
    uint32 DisplayFramebuffer() const {
        return m_displayFB;
    }

private:
    Memory m_mem;
    uint16 m_raw2[0x100] = {}; // the VDP2's registers as written
    Vdp1Regs m_regs1;
    Vdp2Regs m_regs2;
    Vdp1Context m_ctx;
    uint32 m_displayFB = 1; // which framebuffer the VDP2 shows; the other is drawn to
    Reference *m_ref = nullptr;
    std::vector<uint32> m_refColor = std::vector<uint32>(704);
    std::vector<uint8> m_refPriority = std::vector<uint8>(704), m_refSpecial = std::vector<uint8>(704),
                       m_refShadowOrWindow = std::vector<uint8>(704);

    uint16 Vram2Word(uint32 address) const {
        return Be16(&m_mem.vram2[address & 0x7FFFE]);
    }
    uint32 Vram2Long(uint32 address) const {
        return Be32(&m_mem.vram2[address & 0x7FFFC]);
    }
    // (with 24-bit colours, colour RAM keeps the two words of a colour 0x800 bytes apart)
    uint16 CramWord(uint32 address) const {
        address &= 0xFFE;
        if (m_regs2.vramControl.colorRAMMode >= 2) {
            address = ((address & 2) << 10) | ((address >> 1) & 0x7FE);
        }
        return Be16(&m_mem.cram[address]);
    }
    uint32 CramLong(uint32 address) const {
        return (static_cast<uint32>(CramWord(address)) << 16) | CramWord(address + 2);
    }

    std::vector<Command> m_commands;     // of the frame being drawn
    std::vector<Command> m_lastCommands; // of the frame before

    bool m_drawing = false; // between VDP1BeginFrame and VDP1EndFrame
    uint64_t m_drawings = 0;
    uint64_t m_vramWritesWhileDrawing = 0;

    // --- the VDP1's commands rasterised at the original resolution (raster.cpp) ---
    struct Texture { // of a sprite command
        uint32 address;
        uint32 width;
        uint16 mode, color;
        bool flipH;
    };
    uint8 *m_rasterFB = nullptr; // the framebuffer being drawn to
    std::vector<uint8> m_shadowFB[2] = {std::vector<uint8>(0x40000), std::vector<uint8>(0x40000)}; // in a check: drawn here
    uint64_t m_rasterFrames = 0, m_rasterFramesDifferent = 0, m_rasterBytesDifferent = 0;
    uint8 *RasterTarget(uint32 index);
    bool RasterClipped(sint32 x, sint32 y, uint16 mode) const;
    bool RasterPixel(sint32 x, sint32 y, uint16 mode, uint16 color, const Color555 *gouraud);
    bool RasterLineOutside(sint32 x1, sint32 y1, sint32 x2, sint32 y2) const;
    bool RasterLine(CoordS32 from, CoordS32 to, uint16 mode, uint16 color, Color555 gouraudFrom,
                    Color555 gouraudTo, bool antiAlias);
    bool RasterTexturedLine(CoordS32 from, CoordS32 to, const Texture &texture, uint32 row,
                            Color555 gouraudFrom, Color555 gouraudTo);
    void RasterQuad(const Command &cmd, CoordS32 a, CoordS32 b, CoordS32 c, CoordS32 d,
                    bool textured);
    void RasterCommand(const Command &cmd);
    void RasterErase(uint64 cycles);

    // --- for a GPU backend (vdp1.cpp) ---
    bool m_gpu = false, m_gpuCheck = false;
    uint64_t m_gpuPixels = 0, m_gpuSame = 0, m_gpuOnlyHere = 0, m_gpuOnlyReference = 0, m_gpuClose = 0, m_gpuOther = 0;
    std::function<void(const Frame &)> m_onGpuFrame;
    Frame m_frame;
    uint32 m_drawFirstQuad = 0;
    uint64_t m_gpuNotDone = 0; // commands in a drawing mode not done on the GPU
    std::vector<uint32> m_layerImage[5], m_backImage, m_colorImage;
    std::vector<uint8> m_spriteWindowImage;
    void Vdp1Command(const Command &cmd);
    void Vdp1AddQuad(const Command &cmd, const int corners[4][2], uint16 flags, const uint16 *gouraud);
    void Vdp1AddLine(const Command &cmd, const int from[2], const int to[2], uint16 gouraudFrom, uint16 gouraudTo);
    void Vdp1FinishQuad(Quad &quad, const Command &cmd, uint16 flags, const uint16 *gouraud);
    void Vdp1Erase();
    void Vdp1Swap();
    void Vdp1BeginDraw();
    void Vdp1EndDraw();
    void GpuLine(uint32 y);
    void GpuRow(uint32 row, uint32 y, bool otherField);
    bool m_gpuFloor = true;            // the backend draws the rotation screen itself where it can
    std::vector<float> m_floorLines;   // 8 floats a line (FloorLine), by line of a field
    std::vector<uint8> m_vram2Image;   // VDP2 video memory as it was at the end of the frame
    bool m_bothFields = true; // of an interlaced picture, draw the other field's lines too every frame
    void GpuEndFrame();

    void ComposeLine(uint32 y);
    // The sprite layer's line: what the VDP2 makes of the VDP1 framebuffer's pixels (colour, priority, shadow).
    // False if the settings are ones not handled here yet (the reference's line is used then).
    bool SpriteLine(uint32 y);

    // --- scroll screens (layers.cpp) ---
    static constexpr uint32 kMaxLine = 704 + 2 * 256;
    struct LayerLine {
        std::vector<uint32> color = std::vector<uint32>(kMaxLine);
        std::vector<uint8> priority = std::vector<uint8>(kMaxLine); // 0: no pixel
    };
    struct Scroll { // of a normal screen: where the line starts in its map (11.8 fixed point) and the step per pixel
        uint32 fracX = 0, fracY = 0, incH = 0x100, lineTable = 0;
    };
    struct Char { // a pattern name
        uint16 number = 0;
        uint8 palette = 0;
        bool specialPriority = false, flipH = false, flipV = false;
    };
    struct Fetcher { // the pattern name being shown and the one read after it
        Char current, next;
        uint32 lastChar = 0xFFFFFFFF;
        uint8 lastCellX = 0xFF;
    };
    struct Rotation { // of a rotation parameter set: its running start values, and this line's map positions
        sint32 Xst = 0, Yst = 0;
        uint32 KA = 0;
        bool readXst = false, readYst = false, readKAst = false;
        std::vector<sint32> x = std::vector<sint32>(352), y = std::vector<sint32>(352);
        std::vector<uint8> transparent = std::vector<uint8>(352);
        double from[2] = {}, to[2] = {}; // where the line's first and last dots look, with fractions
        bool straight = true;            // ... and the dots between lie on the straight line between
    };
    LayerLine m_layer[6]; // sprites (not used), RBG0, NBG0..NBG3
    Scroll m_scroll[4];
    Fetcher m_fetch[6]; // NBG0..3, rotation parameter sets A and B
    Rotation m_rot[2];
    std::vector<uint8> m_window[5] = {std::vector<uint8>(kMaxLine), std::vector<uint8>(kMaxLine), std::vector<uint8>(kMaxLine),
                                      std::vector<uint8>(kMaxLine), std::vector<uint8>(kMaxLine)}; // RBG0, NBG0..3: 1 = cut out
    std::vector<uint8> m_rotWindow = std::vector<uint8>(kMaxLine), m_spriteWindow = std::vector<uint8>(kMaxLine);
    // Widescreen: extra VDP1 pixels per side; in VDP2 dots (twice as many in the high resolution modes); the
    // width of a layer's line with them
    uint32 m_wideExtra = 0;
    uint32 WideDots() const {
        return m_gpu ? m_wideExtra << (m_width > 352 ? 1 : 0) : 0;
    }
    uint32 RotationWideDots() const { // (a dot of the rotation screen is as wide as one of the VDP1's)
        return m_gpu ? m_wideExtra : 0;
    }
    uint32 LineWidth() const {
        return m_width + 2 * WideDots();
    }
    std::vector<WideSplit> m_wideSplits;
    void SplitLine(uint32 layer, uint32 row, uint32 *line) const;
    void WidenWindow(std::vector<uint8> &line) const;
    void WidenQuad(Quad &quad) const;
    uint32 m_width = 320, m_height = 224;
    uint32 m_backColor = 0;
    uint64_t m_layerPixelsDifferent[6] = {}, m_layerLinesLeftToReference[6] = {}, m_layerLines[6] = {};
    uint64_t m_approximateLines[7] = {}; // sprites, RBG0, NBG0..3, composition: lines that asked for something not done here

    void LayersBeginFrame();
    void LayersBeforeLine();
    void LayersPrepareLine(uint32 y);
    void LayersFinishLine();
    template <typename Set>
    void WindowLine(uint32 row, const Set &set, std::vector<uint8> &out);
    void SpriteWindowLine(uint32 row);
    void WindowLines(uint32 row);
    uint32 Color(uint32 index) const;
    void Colors(uint32 *out) const; // colour RAM as 2048 colours
    void Dot(const BGParams &bg, bool bitmap, uint32 base, uint32 pitch, uint32 dotX, uint32 dotY, uint32 palette,
             bool specialPriority, uint32 &color, uint8 &priority);
    Char OneWordChar(const BGParams &bg, uint16 word) const;
    void MapPixel(const BGParams &bg, bool rot, std::span<const uint32> planes, uint32 pageShiftH, uint32 pageShiftV,
                  uint32 x, uint32 y, Fetcher &fetcher, uint32 &color, uint8 &priority);
    void CellPixel(const BGParams &bg, const Char &ch, uint32 dotX, uint32 dotY, uint32 cell, uint32 &color,
                   uint8 &priority);
    // (offsetY: added to the line's place in the map, 11.8 fixed point; for the other field's line)
    bool NormalLine(uint32 index, sint32 offsetY = 0);
    bool Coefficient(const RotationParams &params, uint32 address, bool always, sint32 &value, bool &transparent) const;
    void RotationParameters(uint32 y);
    bool RotationLine();
    bool FloorLine(float out[8]);
    void FloorSettings(Frame &frame) const;

    struct Line {
        std::vector<uint32> color;
        std::vector<uint8> priority;       // 0: no pixel
        std::vector<uint8> special;        // SpriteSpecial
        std::vector<uint8> shadowOrWindow; // the sprite data's shadow / window bit
    } m_sprite;
    uint64_t m_spritePixelsDifferent = 0, m_spriteLinesLeftToReference = 0;
    unsigned m_debugLeft = 12;

    bool m_compose = false; // the VDP2's side is done here
    bool m_check = false;   // ... and the reference's too, for comparison
    bool m_exclusive = false;
    FrameCallback m_onFrame;
    std::vector<uint32> m_framebuffer;
    // Comparison with the reference
    uint64_t m_frames = 0, m_framesDifferent = 0, m_pixelsDifferent = 0;
    uint64_t m_lines = 0, m_linesLeftToReference = 0;
};

} // namespace satrecomp::video
