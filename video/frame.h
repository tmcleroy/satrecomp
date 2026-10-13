// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// What the renderer hands to a GPU backend for every frame: the VDP1's work since the frame before (its
// drawing commands as quads, framebuffer erases and swaps) and the VDP2's layers, with what is needed to put
// them together. Nothing here depends on a graphics API.
#pragma once

#include <cstdint>
#include <vector>

namespace satrecomp::video {

// One VDP1 drawing command, ready to draw. Positions are in VDP1 framebuffer pixels, a pixel's centre at .5.
struct Quad {
    float corner[4][2]; // A, B, C, D, moved outwards by half a pixel so the pixels the hardware touches are inside
    float clip[4];      // only inside this rectangle (x0, y0, x1, y1; system and user clipping)
    float clipOut[4];   // and outside this one (user clipping in "outside" mode; empty otherwise)
    uint32_t texture;   // address of the character data in VRAM
    uint16_t width, height;
    uint16_t color; // CMDCOLR: the colour of a polygon, the colour bank or table of a sprite
    uint16_t mode;  // CMDPMOD
    uint16_t flags; // kQuad...
    uint16_t lines; // how many lines the hardware draws it in (the longer of its edges A-D and B-C, in pixels)
    uint16_t gouraud[4];
};
enum : uint16_t {
    kQuadFlat = 1,
    kQuadFlipH = 2,
    kQuadFlipV = 4,
    kQuadErase = 8,
    kQuadOddColumns = 16, // when shrinking at high speed, the odd texture columns are the ones used
};

struct Vdp1Event {
    enum class Kind { Erase, Swap, Draw } kind;
    uint32_t firstQuad = 0, quadCount = 0; // Erase: one quad; Draw: the commands of a frame
    uint32_t vram = 0;                     // Draw: which VRAM snapshot the quads' addresses refer to
};

struct Frame {
    // VDP1
    std::vector<Vdp1Event> events;
    std::vector<Quad> quads;
    std::vector<std::vector<uint8_t>> vram; // 512 KiB each
    uint32_t spriteWidth = 352, spriteHeight = 224; // the part of the VDP1 framebuffer that is shown
    bool resetSprites = false; // the backend's framebuffers are of another moment (a state was loaded): empty them first

    // VDP2: the picture's size, and for every layer an image of that size, colour in RGB and priority (0: no
    // pixel) in A. Rows are those of the picture (an interlaced picture keeps the other field's rows).
    uint32_t width = 0, height = 0;
    bool interlaced = false;
    // Widescreen: the picture is wider than the hardware's on both sides (the sizes above include that). In a
    // scene that is not meant to be seen beyond the hardware's picture, this many columns on each side show
    // black instead.
    uint32_t blankSides = 0;
    const uint32_t *layers[5] = {}; // RBG0, NBG0..NBG3
    const uint8_t *spriteWindow = nullptr; // 1 where the sprite layer is cut out
    const uint32_t *back = nullptr;        // per row: the back screen's colour; A = 0 where the display is off
    const uint32_t *colors = nullptr;      // colour RAM as 2048 colours

    // The rotation screen (the floor, in most 3D games), for a backend to draw at its own resolution. Per line
    // of a field 8 floats: where the line's first and last dots look in the map (x, y, x, y; in dots), then
    // 1 if the line shows the map there, 0 if nothing, 2 if the line is in layers[0] instead (drawn by the
    // renderer because of something a backend does not do).
    struct Floor {
        const float *lines = nullptr;
        uint32_t lineCount = 0, dots = 0; // lines of a field, dots a line
        const uint8_t *vram = nullptr;    // VDP2 video memory, 512 KiB
        uint32_t planes[16] = {};         // address of each of the map's 4x4 planes
        bool fourCell = false, twoWord = false, extended = false, colors256 = false, transparency = true;
        bool priorityPerCharacter = false, specialPriority = false;
        uint8_t over = 0; // outside the map: 0 it repeats, 1 nothing, 2 nothing outside the first 512x512
        uint8_t pageShiftH = 0, pageShiftV = 0, priority = 0, access = 0xFF;
        uint32_t colorOffset = 0, moreNumber = 0, morePalette = 0;
    } floor;

    // How the VDP2 reads the sprite framebuffer
    uint32_t spriteType = 0;
    bool spriteMixed = false, spriteWindowUsed = false, spriteWindowEnabled = false, spriteWindowInverted = false;
    bool spritesEnabled = true;
    uint32_t spriteColorOffset = 0;
    uint8_t spritePriorities[8] = {};
    bool shadow[7] = {}; // which layers take sprite shadows: sprites (unused), RBG0, NBG0..3, back
    int16_t colorOffset[7][3] = {}; // added to the colour (0..255 a channel) of the layer shown: sprites, RBG0, NBG0..3, back
};

} // namespace satrecomp::video
