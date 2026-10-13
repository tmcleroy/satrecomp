// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The VDP1's drawing commands rasterised into its framebuffer at the original resolution, as the hardware
// does it: a quad is drawn as lines from its edge A-D to its edge B-C, every line pixel by pixel, a slanted
// line with an extra pixel at each step so that neighbouring lines leave no holes. Pixel for pixel what the
// reference renderer draws (SATRECOMP_VIDEO=check compares the framebuffers after every frame).
//
// The counters that step along edges, lines, textures and shading are in counters.h; what is drawn with
// them is done here.
#include "renderer.h"

#include "counters.h"

#include <algorithm>
#include <limits>

namespace satrecomp::video {

namespace {
    int Coordinate(uint16 word) {
        return bit::sign_extend<13>(word);
    }

    // CMDPMOD
    constexpr uint16 kGouraud = 0x0004, kNoTransparent = 0x0040, kNoEndCodes = 0x0080, kMesh = 0x0100, kClipOutside = 0x0200,
                     kUserClip = 0x0400, kHighSpeedShrink = 0x1000, kMsbOn = 0x8000;

    Color555 Shade(Color555 color, Color555 gouraud) {
        auto channel = [](int c, int g) { return static_cast<uint16>(std::clamp(c + g - 16, 0, 31)); };
        return Color555{.r = channel(color.r, gouraud.r), .g = channel(color.g, gouraud.g), .b = channel(color.b, gouraud.b), .msb = color.msb};
    }
} // namespace

uint8 *Renderer::RasterTarget(uint32 index) {
    return m_check ? m_shadowFB[index & 1].data() : m_mem.fb[index & 1];
}

SV_INLINE bool Renderer::RasterClipped(sint32 x, sint32 y, uint16 mode) const {
    const auto &ctx = m_ctx;
    if (x < 0 || x > ctx.sysClipH || y < 0 || y > ctx.sysClipV) {
        return true;
    }
    if (mode & kUserClip) {
        const bool outside = x < ctx.userClipX0 || x > ctx.userClipX1 || y < ctx.userClipY0 || y > ctx.userClipY1;
        if (outside != static_cast<bool>(mode & kClipOutside)) {
            return true;
        }
    }
    return false;
}

// One pixel. False if it was clipped (a line ends where its pixels start being clipped).
SV_INLINE bool Renderer::RasterPixel(sint32 x, sint32 y, uint16 mode, uint16 color, const Color555 *gouraud) {
    if (RasterClipped(x, y, mode)) {
        return false;
    }
    if ((mode & kMesh) && ((x ^ y) & 1)) {
        return true;
    }
    const Vdp1Regs &regs1 = m_regs1;
    if (regs1.dblInterlaceEnable) {
        // (only the lines of the field being shown, when the framebuffer holds an interlaced picture)
        if (m_regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity && (y & 1) != regs1.dblInterlaceDrawLine) {
            return true;
        }
        y >>= 1;
    }
    uint32 offset = y * regs1.fbSizeH + x;
    if (!regs1.pixel8Bits) {
        offset *= 2;
    }
    offset &= 0x3FFFF;
    uint8 *fb = m_rasterFB;
    if (mode & kMsbOn) {
        fb[offset & ~1u] |= 0x80;
        return true;
    }
    if (regs1.pixel8Bits) {
        fb[offset] = static_cast<uint8>(color);
        return true;
    }
    Color555 source{.u16 = color};
    Color555 target{.u16 = static_cast<uint16>((fb[offset] << 8) | fb[offset + 1])};
    if (gouraud) {
        source = Shade(source, *gouraud);
    }
    switch (mode & 3) {
    case 0: target = source; break;
    case 1: // shadow: what is there at half brightness
        if (target.msb) {
            target.r >>= 1;
            target.g >>= 1;
            target.b >>= 1;
        }
        break;
    case 2: // half luminance
        target.r = source.r >> 1;
        target.g = source.g >> 1;
        target.b = source.b >> 1;
        target.msb = source.msb;
        break;
    default: // half transparent: the average with what is there
        if (target.msb) {
            target.r = (source.r + target.r) >> 1;
            target.g = (source.g + target.g) >> 1;
            target.b = (source.b + target.b) >> 1;
        } else {
            target = source;
        }
        break;
    }
    fb[offset] = static_cast<uint8>(target.u16 >> 8);
    fb[offset + 1] = static_cast<uint8>(target.u16);
    return true;
}

bool Renderer::RasterLineOutside(sint32 x1, sint32 y1, sint32 x2, sint32 y2) const {
    const auto &ctx = m_ctx;
    return (x1 < 0 && x2 < 0) || (y1 < 0 && y2 < 0) || (x1 > ctx.sysClipH && x2 > ctx.sysClipH) || (y1 > ctx.sysClipV && y2 > ctx.sysClipV);
}

// A line of one colour. True if any of it was inside the drawing area.
bool Renderer::RasterLine(CoordS32 from, CoordS32 to, uint16 mode, uint16 color, Color555 gouraudFrom, Color555 gouraudTo, bool antiAlias) {
    if (RasterLineOutside(from.x(), from.y(), to.x(), to.y())) {
        return false;
    }
    const auto &ctx = m_ctx;
    LineWalk line{from, to, antiAlias};
    const uint32 skipped = line.Clip(ctx.sysClipH, ctx.sysClipV);
    Gradient gouraud;
    const bool shaded = mode & kGouraud;
    if (shaded) {
        gouraud.Begin(line.Span() + 1, gouraudFrom, gouraudTo);
        gouraud.Skip(skipped);
    }
    // (the pixel a line starts on never has a second one)
    bool extra = false;
    bool plotted = false;
    for (line.Move(); line.More(); extra = line.Move()) {
        const Color555 shade = shaded ? gouraud.Now() : Color555{};
        bool drawn = RasterPixel(line.At().x(), line.At().y(), mode, color, shaded ? &shade : nullptr);
        if (extra) {
            drawn |= RasterPixel(line.Beside().x(), line.Beside().y(), mode, color, shaded ? &shade : nullptr);
        }
        if (drawn) {
            plotted = true;
        } else if (plotted && !(mode & kClipOutside)) {
            break; // (nothing more of it can be inside)
        }
        if (shaded) {
            gouraud.Step();
        }
    }
    return plotted;
}

// A line of a sprite: one row of the character, stretched or shrunk over the line's pixels
bool Renderer::RasterTexturedLine(CoordS32 from, CoordS32 to, const Texture &texture, uint32 row, Color555 gouraudFrom, Color555 gouraudTo) {
    if (RasterLineOutside(from.x(), from.y(), to.x(), to.y())) {
        return false;
    }
    const auto &ctx = m_ctx;
    const auto &vram = m_mem.vram1;
    const uint16 mode = texture.mode;
    const uint32 width = std::max<uint32>(texture.width, 1);
    auto byte = [&](uint32 address) -> uint32 { return vram[address & 0x7FFFF]; };
    auto word = [&](uint32 address) -> uint32 { return (vram[address & 0x7FFFE] << 8) | vram[(address & 0x7FFFE) + 1]; };

    LineWalk line{from, to, true};
    const uint32 skipped = line.Clip(ctx.sysClipH, ctx.sysClipV);
    Gradient gouraud;
    const bool shaded = mode & kGouraud;
    if (shaded) {
        gouraud.Begin(line.Span() + 1, gouraudFrom, gouraudTo);
        gouraud.Skip(skipped);
    }

    sint32 first = 0, last = static_cast<sint32>(width) - 1;
    if (texture.flipH) {
        std::swap(first, last);
    }
    // (shrinking to less than half, the hardware can skip every other column)
    const bool halved = (mode & kHighSpeedShrink) && line.Span() < width - 1;
    TexelWalk column;
    column.Begin(line.Span() + 1, first, last, halved, m_regs1.evenOddCoordSelect);
    column.SkipPixels(skipped);

    uint16 color = 0;
    bool transparent = true;
    bool endCode = false;
    // (the second end code in a row ends the line; not when columns are skipped)
    int endCodes = halved ? std::numeric_limits<int>::min() : 0;
    auto texel = [&] {
        const uint32 u = column.At();
        const uint32 index = u + row * width;
        bool isEnd = false;
        switch ((mode >> 3) & 7) {
        case 0: // 16 colours, in a colour bank
            color = (byte(texture.address + (index >> 1)) >> ((~u & 1) * 4)) & 0xF;
            isEnd = color == 0xF;
            transparent = color == 0;
            color |= texture.color & 0xFFF0;
            break;
        case 1: // 16 colours, through a table in video memory
            color = (byte(texture.address + (index >> 1)) >> ((~u & 1) * 4)) & 0xF;
            isEnd = color == 0xF;
            transparent = color == 0;
            color = static_cast<uint16>(word(color * 2 + (static_cast<uint32>(texture.color) << 3)));
            break;
        case 2:
            color = static_cast<uint16>(byte(texture.address + index));
            isEnd = color == 0xFF;
            transparent = color == 0;
            color = (color & 0x3F) | (texture.color & 0xFFC0);
            break;
        case 3:
            color = static_cast<uint16>(byte(texture.address + index));
            isEnd = color == 0xFF;
            transparent = color == 0;
            color = (color & 0x7F) | (texture.color & 0xFF80);
            break;
        case 4:
            color = static_cast<uint16>(byte(texture.address + index));
            isEnd = color == 0xFF;
            transparent = color == 0;
            color |= texture.color & 0xFF00;
            break;
        case 5: // RGB
            color = static_cast<uint16>(word(texture.address + index * 2));
            isEnd = color == 0x7FFF;
            transparent = !(color & 0x8000);
            break;
        default: break;
        }
        if (isEnd && !(mode & kNoEndCodes)) {
            endCode = true;
            ++endCodes;
        } else {
            endCode = false;
        }
    };
    texel();

    bool extra = false;
    bool plotted = false;
    for (line.Move(); line.More(); extra = line.Move()) {
        while (column.Behind()) {
            column.Advance();
            texel();
            if (endCodes == 2) {
                break;
            }
        }
        if (endCodes == 2) {
            break;
        }
        column.PixelDone();
        const sint32 x = line.At().x(), y = line.At().y();

        if (endCode || (transparent && !(mode & kNoTransparent))) {
            if (shaded) {
                gouraud.Step();
            }
            // (a pixel not drawn still counts as inside the drawing area if it is)
            if (!(mode & kClipOutside)) {
                if (!RasterClipped(x, y, mode) || (extra && !RasterClipped(line.Beside().x(), line.Beside().y(), mode))) {
                    plotted = true;
                    continue;
                }
                if (plotted) {
                    break;
                }
            }
            continue;
        }

        const Color555 shade = shaded ? gouraud.Now() : Color555{};
        bool drawn = RasterPixel(x, y, mode, color, shaded ? &shade : nullptr);
        if (extra) {
            drawn |= RasterPixel(line.Beside().x(), line.Beside().y(), mode, color, shaded ? &shade : nullptr);
        }
        if (drawn) {
            plotted = true;
        } else if (plotted && !(mode & kClipOutside)) {
            break;
        }
        if (shaded) {
            gouraud.Step();
        }
    }

    if (endCodes == 2 && !plotted && !(mode & kClipOutside)) {
        // (cut short by end codes before reaching the drawing area: whether the rest would have been inside
        // still decides if the quad goes on)
        for (; line.More(); extra = line.Move()) {
            if (!RasterClipped(line.At().x(), line.At().y(), mode) ||
                (extra && !RasterClipped(line.Beside().x(), line.Beside().y(), mode))) {
                plotted = true;
                break;
            }
        }
    }
    return plotted;
}

// A quad: lines between points stepping down its edges A-D and B-C. Textured with the command's character,
// or of one colour.
void Renderer::RasterQuad(const Command &cmd, CoordS32 a, CoordS32 b, CoordS32 c, CoordS32 d, bool textured) {
    const auto &ctx = m_ctx;
    const sint32 xs[4] = {a.x(), b.x(), c.x(), d.x()}, ys[4] = {a.y(), b.y(), c.y(), d.y()};
    auto all = [](const sint32 *v, auto test) { return test(v[0]) && test(v[1]) && test(v[2]) && test(v[3]); };
    if (all(xs, [](sint32 v) { return v < 0; }) || all(ys, [](sint32 v) { return v < 0; }) ||
        all(xs, [&](sint32 v) { return v > ctx.sysClipH; }) || all(ys, [&](sint32 v) { return v > ctx.sysClipV; })) {
        return;
    }
    const uint16 mode = cmd.words[2];
    const uint16 color = cmd.words[3];
    auto vram = [&](uint32 address) {
        address &= 0x7FFFE;
        return static_cast<uint16>((m_mem.vram1[address] << 8) | m_mem.vram1[address + 1]);
    };

    Texture texture{};
    texture.mode = mode;
    texture.color = color;
    texture.address = static_cast<uint32>(cmd.words[4]) * 8;
    if (((mode >> 3) & 7) == 5) {
        texture.address &= ~0xFu;
    }
    texture.width = ((cmd.words[5] >> 8) & 0x3F) * 8;
    texture.flipH = cmd.words[0] & 0x10;
    const uint32 height = cmd.words[5] & 0xFF;

    Ladder quad{a, b, c, d};
    const bool shaded = mode & kGouraud;
    if (shaded) {
        const uint32 table = static_cast<uint32>(cmd.words[14]) << 3;
        quad.BeginShade(Color555{.u16 = vram(table)}, Color555{.u16 = vram(table + 2)}, Color555{.u16 = vram(table + 4)},
                          Color555{.u16 = vram(table + 6)});
    }
    TexelWalk rows;
    if (textured) {
        // (a character of no width shows its first texel all over)
        quad.BeginRows(rows, texture.width == 0 ? 1 : height, cmd.words[0] & 0x20);
    }

    // Once the lines have left the drawing area nothing more of the quad can be inside it (twice, for the quads
    // Ladder::TwoRuns names)
    bool inside = false;
    int runs = 0;
    const int maxRuns = quad.TwoRuns() ? 2 : 1;
    for (; quad.More(); quad.Step()) {
        const CoordS32 left = quad.Left().At(), right = quad.Right().At();
        const Color555 shadeLeft = shaded ? quad.Left().ShadeNow() : Color555{};
        const Color555 shadeRight = shaded ? quad.Right().ShadeNow() : Color555{};
        bool plotted;
        if (textured) {
            while (rows.Behind()) {
                rows.Advance();
            }
            const uint32 row = rows.At();
            rows.PixelDone();
            plotted = RasterTexturedLine(left, right, texture, row, shadeLeft, shadeRight);
        } else {
            plotted = RasterLine(left, right, mode, color, shadeLeft, shadeRight, true);
        }
        if (plotted) {
            if (!inside) {
                inside = true;
                ++runs;
            }
        } else if (runs >= maxRuns) {
            break;
        } else {
            inside = false;
        }
    }
}

void Renderer::RasterCommand(const Command &cmd) {
    auto &ctx = m_ctx;
    const uint32 type = cmd.words[0] & 0xF;
    switch (type) {
    case 8:
    case 11:
        ctx.userClipX0 = cmd.words[6];
        ctx.userClipY0 = cmd.words[7];
        ctx.userClipX1 = cmd.words[10];
        ctx.userClipY1 = cmd.words[11];
        return;
    case 9:
        ctx.sysClipH = cmd.words[10];
        ctx.sysClipV = cmd.words[11];
        return;
    case 10:
        ctx.localCoordX = Coordinate(cmd.words[6]);
        ctx.localCoordY = Coordinate(cmd.words[7]);
        return;
    default: break;
    }
    if (type > 7 || !m_regs2.layerEnabled[0]) {
        return;
    }
    m_rasterFB = RasterTarget((m_displayFB ^ 1u));

    const uint16 mode = cmd.words[2];
    const sint32 lx = ctx.localCoordX, ly = ctx.localCoordY;
    CoordS32 p[4];
    for (int i = 0; i < 4; i++) {
        p[i] = {Coordinate(cmd.words[6 + i * 2]) + lx, Coordinate(cmd.words[7 + i * 2]) + ly};
    }
    auto vram = [&](uint32 address) {
        address &= 0x7FFFE;
        return Color555{.u16 = static_cast<uint16>((m_mem.vram1[address] << 8) | m_mem.vram1[address + 1])};
    };
    const uint32 table = static_cast<uint32>(cmd.words[14]) << 3;

    switch (type) {
    case 0: { // normal sprite
        const sint32 width = std::max(((cmd.words[5] >> 8) & 0x3F) * 8, 1), height = std::max(cmd.words[5] & 0xFF, 1);
        const sint32 x0 = p[0].x(), y0 = p[0].y(), x1 = x0 + width - 1, y1 = y0 + height - 1;
        RasterQuad(cmd, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, true);
        break;
    }
    case 1: { // scaled sprite
        const sint32 xa = Coordinate(cmd.words[6]), ya = Coordinate(cmd.words[7]);
        sint32 x0 = xa, x1 = xa, y0 = ya, y1 = ya;
        const uint32 zoomH = (cmd.words[0] >> 8) & 3, zoomV = (cmd.words[0] >> 10) & 3;
        if (zoomH == 0) {
            x1 = Coordinate(cmd.words[10]);
        } else {
            const sint32 size = Coordinate(cmd.words[8]);
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
            const sint32 size = Coordinate(cmd.words[9]);
            switch (zoomV) {
            case 1: y1 += size; break;
            case 2:
                y0 -= size >> 1;
                y1 += (size + 1) >> 1;
                break;
            default: y0 -= size; break;
            }
        }
        RasterQuad(cmd, {x0 + lx, y0 + ly}, {x1 + lx, y0 + ly}, {x1 + lx, y1 + ly}, {x0 + lx, y1 + ly}, true);
        break;
    }
    case 2:
    case 3: RasterQuad(cmd, p[0], p[1], p[2], p[3], true); break;
    case 4: RasterQuad(cmd, p[0], p[1], p[2], p[3], false); break;
    case 5:
    case 7: { // polyline
        const sint32 xs[4] = {p[0].x(), p[1].x(), p[2].x(), p[3].x()}, ys[4] = {p[0].y(), p[1].y(), p[2].y(), p[3].y()};
        auto all = [](const sint32 *v, auto test) { return test(v[0]) && test(v[1]) && test(v[2]) && test(v[3]); };
        if (all(xs, [](sint32 v) { return v < 0; }) || all(ys, [](sint32 v) { return v < 0; }) ||
            all(xs, [&](sint32 v) { return v > ctx.sysClipH; }) || all(ys, [&](sint32 v) { return v > ctx.sysClipV; })) {
            break;
        }
        Color555 shade[4];
        for (int i = 0; i < 4; i++) {
            shade[i] = vram(table + i * 2);
        }
        for (int i = 0; i < 4; i++) {
            RasterLine(p[i], p[(i + 1) & 3], mode, cmd.words[3], shade[i], shade[(i + 1) & 3], false);
        }
        break;
    }
    case 6: // line
        RasterLine(p[0], p[1], mode, cmd.words[3], vram(table), vram(table + 2), false);
        break;
    default: break;
    }
}

// The framebuffer on display set to one value inside the erase rectangle (it is drawn to next)
void Renderer::RasterErase(uint64 cycles) {
    const Vdp1Regs &regs1 = m_regs1;
    const Vdp2Regs &regs2 = m_regs2;
    uint8 *fb = RasterTarget(m_displayFB);
    const uint32 scaleV = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity ? 1 : 0;
    const uint32 maxH = (regs2.TVMD.HRESOn & 1) ? 428 : 400;
    const uint32 maxV = m_height >> scaleV;
    const uint32 x1 = std::min<uint32>(regs1.eraseX1Latch, maxH), x3 = std::min<uint32>(regs1.eraseX3Latch, maxH);
    const uint32 y1 = std::min<uint32>(regs1.eraseY1Latch, maxV) << scaleV, y3 = std::min<uint32>(regs1.eraseY3Latch, maxV) << scaleV;
    const bool limited = cycles != 0; // (erasing outside the blanking period has only so much time)
    const uint8 high = static_cast<uint8>(regs1.eraseWriteValueLatch >> 8), low = static_cast<uint8>(regs1.eraseWriteValueLatch);
    for (uint32 y = y1; y <= y3; y++) {
        const uint32 rowOffset = y << regs1.eraseOffsetShift;
        for (uint32 x = x1; x < x3; x++) {
            const uint32 address = ((rowOffset + x) * 2) & 0x3FFFE;
            fb[address] = high;
            fb[address + 1] = low;
            if (limited) {
                if (cycles == 0) {
                    return;
                }
                cycles--;
            }
        }
    }
}

} // namespace satrecomp::video
