// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The VDP1's drawing commands made ready for a GPU: every sprite, polygon and line becomes a quad (frame.h).
//
// The hardware draws a quad as lines from its edge A-D to its edge B-C, stepping along both, and takes a
// texture column per step along a line: the texture is stretched over the quad bilinearly. A GPU backend finds
// for each pixel where in the quad it lies, by the same bilinear rule, and so draws the same shape at any
// resolution.
//
// Not done: the drawing modes that read what is already in the framebuffer (shadow, half transparency, MSB
// on), end codes cutting a line short, and the exact choice of texture columns when shrinking.
#include "renderer.h"

#include <algorithm>
#include <cmath>

namespace satrecomp::video {

namespace {
    int Coordinate(uint16 word) {
        return bit::sign_extend<13>(word);
    }
} // namespace

void Renderer::Vdp1AddQuad(const Command &cmd, const int corners[4][2], uint16 flags, const uint16 *gouraud) {
    const auto &ctx = m_ctx;
    // Nothing if the whole quad is off one side of the drawing area
    const int extra = static_cast<int>(m_wideExtra);
    const int maxX = ctx.sysClipH, maxY = ctx.sysClipV;
    bool left = true, right = true, above = true, below = true;
    for (int i = 0; i < 4; i++) {
        left = left && corners[i][0] < -extra;
        right = right && corners[i][0] > maxX + extra;
        above = above && corners[i][1] < 0;
        below = below && corners[i][1] > maxY;
    }
    if (left || right || above || below) {
        return;
    }

    Quad &quad = m_frame.quads.emplace_back();
    // The corners are pixels the hardware draws: the quad reaches half a pixel beyond their centres, away from
    // its middle
    float cx = 0, cy = 0;
    for (int i = 0; i < 4; i++) {
        cx += corners[i][0] * 0.25f;
        cy += corners[i][1] * 0.25f;
    }
    static const float kSideX[4] = {-1, 1, 1, -1}, kSideY[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; i++) {
        const float dx = corners[i][0] - cx, dy = corners[i][1] - cy;
        const float sx = std::fabs(dx) < 0.01f ? kSideX[i] : (dx < 0 ? -1.0f : 1.0f);
        const float sy = std::fabs(dy) < 0.01f ? kSideY[i] : (dy < 0 ? -1.0f : 1.0f);
        quad.corner[i][0] = corners[i][0] + 0.5f + 0.5f * sx;
        quad.corner[i][1] = corners[i][1] + 0.5f + 0.5f * sy;
    }
    Vdp1FinishQuad(quad, cmd, flags, gouraud);
    int lines = 0;
    for (int i = 0; i < 2; i++) { // edges A-D and B-C
        lines = std::max({lines, std::abs(corners[3 - i][0] - corners[i][0]), std::abs(corners[3 - i][1] - corners[i][1])});
    }
    quad.lines = static_cast<uint16>(std::min(lines + 1, 0x3FFF));
    WidenQuad(quad);
}

// Widescreen: the drawing area has more columns on both sides. Everything moves right by that much, and
// clipping at the hardware picture's left or right edge becomes clipping at the wider area's.
void Renderer::WidenQuad(Quad &quad) const {
    const float extra = static_cast<float>(m_wideExtra);
    if (extra == 0) {
        return;
    }
    const float width = static_cast<float>(m_width > 352 ? m_width >> 1 : m_width);
    for (auto &corner : quad.corner) {
        corner[0] += extra;
    }
    quad.clip[0] = quad.clip[0] <= 0 ? 0 : quad.clip[0] + extra;
    quad.clip[2] = quad.clip[2] >= width ? width + 2 * extra : quad.clip[2] + extra;
    if (quad.clipOut[2] > quad.clipOut[0]) {
        quad.clipOut[0] += extra;
        quad.clipOut[2] += extra;
    }
}

void Renderer::Vdp1AddLine(const Command &cmd, const int from[2], const int to[2], uint16 gouraudFrom, uint16 gouraudTo) {
    // A line is a quad one pixel thick around it
    float dx = static_cast<float>(to[0] - from[0]), dy = static_cast<float>(to[1] - from[1]);
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.01f) {
        dx = 1;
        dy = 0;
    } else {
        dx /= length;
        dy /= length;
    }
    const float nx = -dy * 0.5f, ny = dx * 0.5f;
    const float ax = from[0] + 0.5f - dx * 0.5f, ay = from[1] + 0.5f - dy * 0.5f;
    const float bx = to[0] + 0.5f + dx * 0.5f, by = to[1] + 0.5f + dy * 0.5f;
    Quad &quad = m_frame.quads.emplace_back();
    quad.corner[0][0] = ax - nx;
    quad.corner[0][1] = ay - ny;
    quad.corner[1][0] = bx - nx;
    quad.corner[1][1] = by - ny;
    quad.corner[2][0] = bx + nx;
    quad.corner[2][1] = by + ny;
    quad.corner[3][0] = ax + nx;
    quad.corner[3][1] = ay + ny;
    const uint16 gouraud[4] = {gouraudFrom, gouraudTo, gouraudTo, gouraudFrom};
    Vdp1FinishQuad(quad, cmd, kQuadFlat, gouraud);
    WidenQuad(quad);
}

void Renderer::Vdp1FinishQuad(Quad &quad, const Command &cmd, uint16 flags, const uint16 *gouraud) {
    const auto &ctx = m_ctx;
    const uint16 mode = cmd.words[2];
    quad.clip[0] = 0;
    quad.clip[1] = 0;
    quad.clip[2] = ctx.sysClipH + 1.0f;
    quad.clip[3] = ctx.sysClipV + 1.0f;
    quad.clipOut[0] = quad.clipOut[1] = quad.clipOut[2] = quad.clipOut[3] = 0;
    if (mode & 0x400) { // user clipping: draw only inside, or only outside, the user's rectangle
        if (mode & 0x200) {
            quad.clipOut[0] = ctx.userClipX0;
            quad.clipOut[1] = ctx.userClipY0;
            quad.clipOut[2] = ctx.userClipX1 + 1.0f;
            quad.clipOut[3] = ctx.userClipY1 + 1.0f;
        } else {
            quad.clip[0] = std::max<float>(quad.clip[0], ctx.userClipX0);
            quad.clip[1] = std::max<float>(quad.clip[1], ctx.userClipY0);
            quad.clip[2] = std::min<float>(quad.clip[2], ctx.userClipX1 + 1.0f);
            quad.clip[3] = std::min<float>(quad.clip[3], ctx.userClipY1 + 1.0f);
        }
    }
    const uint32 colorMode = (mode >> 3) & 7;
    quad.texture = static_cast<uint32>(cmd.words[4]) * 8;
    if (colorMode == 5) {
        quad.texture &= ~0xFu;
    }
    quad.width = ((cmd.words[5] >> 8) & 0x3F) * 8;
    quad.height = cmd.words[5] & 0xFF;
    quad.color = cmd.words[3];
    quad.mode = mode;
    quad.flags = flags | (m_regs1.evenOddCoordSelect ? kQuadOddColumns : 0);
    quad.lines = 1;
    for (int i = 0; i < 4; i++) {
        quad.gouraud[i] = gouraud ? gouraud[i] : 0;
    }
    if ((mode & 3) == 1 || (mode & 3) == 3 || (mode & 0x8000)) {
        m_gpuNotDone++; // (drawn as if it replaced what is there)
    }
}

// A command the hardware model has just reached in the game's command table
void Renderer::Vdp1Command(const Command &cmd) {
    auto &ctx = m_ctx;
    const uint32 type = cmd.words[0] & 0xF;
    const uint16 mode = cmd.words[2];
    auto vram = [&](uint32 address) {
        address &= 0x7FFFE;
        return static_cast<uint16>((m_mem.vram1[address] << 8) | m_mem.vram1[address + 1]);
    };
    switch (type) {
    case 8:
    case 11: // user clipping
        ctx.userClipX0 = cmd.words[6];
        ctx.userClipY0 = cmd.words[7];
        ctx.userClipX1 = cmd.words[10];
        ctx.userClipY1 = cmd.words[11];
        return;
    case 9: // system clipping
        ctx.sysClipH = cmd.words[10];
        ctx.sysClipV = cmd.words[11];
        return;
    case 10: // local coordinates
        ctx.localCoordX = Coordinate(cmd.words[6]);
        ctx.localCoordY = Coordinate(cmd.words[7]);
        return;
    default: break;
    }
    if (type > 7 || !m_regs2.layerEnabled[0]) {
        return;
    }

    uint16 gouraud[4] = {};
    const bool shaded = mode & 4;
    if (shaded) {
        const uint32 table = static_cast<uint32>(cmd.words[14]) << 3;
        for (int i = 0; i < 4; i++) {
            gouraud[i] = vram(table + i * 2);
        }
    }
    const int lx = ctx.localCoordX, ly = ctx.localCoordY;
    int c[4][2];
    for (int i = 0; i < 4; i++) {
        c[i][0] = Coordinate(cmd.words[6 + i * 2]) + lx;
        c[i][1] = Coordinate(cmd.words[7 + i * 2]) + ly;
    }
    const uint16 flips = ((cmd.words[0] & 0x10) ? kQuadFlipH : 0) | ((cmd.words[0] & 0x20) ? kQuadFlipV : 0);
    const int width = std::max(((cmd.words[5] >> 8) & 0x3F) * 8, 1), height = std::max(cmd.words[5] & 0xFF, 1);

    switch (type) {
    case 0: { // normal sprite: the character as it is, its top left at A
        const int q[4][2] = {{c[0][0], c[0][1]}, {c[0][0] + width - 1, c[0][1]}, {c[0][0] + width - 1, c[0][1] + height - 1},
                             {c[0][0], c[0][1] + height - 1}};
        Vdp1AddQuad(cmd, q, flips, shaded ? gouraud : nullptr);
        break;
    }
    case 1: { // scaled sprite: a rectangle given by two corners, or by a fixed point and a size
        const int xa = Coordinate(cmd.words[6]), ya = Coordinate(cmd.words[7]);
        int x0 = xa, x1 = xa, y0 = ya, y1 = ya;
        const uint32 zoomH = (cmd.words[0] >> 8) & 3, zoomV = (cmd.words[0] >> 10) & 3;
        if (zoomH == 0) {
            x1 = Coordinate(cmd.words[10]);
        } else {
            const int size = Coordinate(cmd.words[8]);
            switch (zoomH) {
            case 1: x1 += size; break;
            case 2:
                x0 -= size >> 1;
                x1 += (size + 1) >> 1;
                break;
            default: x0 -= size; break;
            }
        }
        if (zoomV == 0) {
            y1 = Coordinate(cmd.words[11]);
        } else {
            const int size = Coordinate(cmd.words[9]);
            switch (zoomV) {
            case 1: y1 += size; break;
            case 2:
                y0 -= size >> 1;
                y1 += (size + 1) >> 1;
                break;
            default: y0 -= size; break;
            }
        }
        const int q[4][2] = {{x0 + lx, y0 + ly}, {x1 + lx, y0 + ly}, {x1 + lx, y1 + ly}, {x0 + lx, y1 + ly}};
        Vdp1AddQuad(cmd, q, flips, shaded ? gouraud : nullptr);
        break;
    }
    case 2:
    case 3: // distorted sprite: the character stretched over any four points
        Vdp1AddQuad(cmd, c, flips, shaded ? gouraud : nullptr);
        break;
    case 4: // polygon: four points filled with one colour
        Vdp1AddQuad(cmd, c, kQuadFlat, shaded ? gouraud : nullptr);
        break;
    case 5:
    case 7: // polyline: the four sides
        for (int i = 0; i < 4; i++) {
            Vdp1AddLine(cmd, c[i], c[(i + 1) & 3], gouraud[i], gouraud[(i + 1) & 3]);
        }
        break;
    case 6: // line
        Vdp1AddLine(cmd, c[0], c[1], gouraud[0], gouraud[1]);
        break;
    default: break;
    }
}

// The hardware erases the framebuffer that was on display (to one value, inside a rectangle) before it becomes
// the one drawn to
void Renderer::Vdp1Erase() {
    const Vdp1Regs &regs1 = m_regs1;
    const Vdp2Regs &regs2 = m_regs2;
    const uint32 scaleV = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity ? 1 : 0;
    const uint32 maxH = (regs2.TVMD.HRESOn & 1) ? 428 : 400;
    const uint32 maxV = m_height >> scaleV;
    const float x1 = std::min<uint32>(regs1.eraseX1Latch, maxH), x3 = std::min<uint32>(regs1.eraseX3Latch, maxH);
    const float y1 = std::min<uint32>(regs1.eraseY1Latch, maxV) << scaleV, y3 = (std::min<uint32>(regs1.eraseY3Latch, maxV) << scaleV) + 1;
    if (x3 <= x1 || y3 <= y1) {
        return;
    }
    Vdp1Event &event = m_frame.events.emplace_back();
    event.kind = Vdp1Event::Kind::Erase;
    event.firstQuad = static_cast<uint32>(m_frame.quads.size());
    event.quadCount = 1;
    Quad &quad = m_frame.quads.emplace_back();
    // (widescreen: an erase that covers the hardware's picture covers the wider one)
    const float wide = x1 == 0 && x3 >= static_cast<float>(m_width > 352 ? m_width >> 1 : m_width) ? 2.0f * m_wideExtra : 0.0f;
    const float rect[4][2] = {{x1, y1}, {x3 + wide, y1}, {x3 + wide, y3}, {x1, y3}};
    for (int i = 0; i < 4; i++) {
        quad.corner[i][0] = rect[i][0];
        quad.corner[i][1] = rect[i][1];
    }
    quad.clip[0] = quad.clip[1] = 0;
    quad.clip[2] = quad.clip[3] = 4096;
    quad.clipOut[0] = quad.clipOut[1] = quad.clipOut[2] = quad.clipOut[3] = 0;
    quad.texture = 0;
    quad.width = quad.height = 0;
    quad.color = regs1.eraseWriteValueLatch;
    quad.mode = 0;
    quad.flags = kQuadFlat | kQuadErase;
    quad.lines = 1;
    quad.gouraud[0] = quad.gouraud[1] = quad.gouraud[2] = quad.gouraud[3] = 0;
}

void Renderer::Vdp1Swap() {
    m_frame.events.push_back({Vdp1Event::Kind::Swap});
}

void Renderer::Vdp1BeginDraw() {
    m_drawFirstQuad = static_cast<uint32>(m_frame.quads.size());
}

// The command table has been gone through: the frame's quads, with video memory as it is now (the characters,
// colour tables)
void Renderer::Vdp1EndDraw() {
    Vdp1Event &event = m_frame.events.emplace_back();
    event.kind = Vdp1Event::Kind::Draw;
    event.firstQuad = m_drawFirstQuad;
    event.quadCount = static_cast<uint32>(m_frame.quads.size()) - m_drawFirstQuad;
    event.vram = static_cast<uint32>(m_frame.vram.size());
    m_frame.vram.emplace_back(m_mem.vram1, m_mem.vram1 + 0x80000);
}

} // namespace satrecomp::video
