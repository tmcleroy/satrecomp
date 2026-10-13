// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// How a VDP1 counts its way across what it draws. The chip has no multiplier or divider in its drawing path: a
// quad is drawn as a ladder of lines between two of its edges, and where the next rung starts, which pixel of a
// line comes next, which texel it shows and how far its shading has moved on are all kept by small counters that
// are added to once per step and give a pulse when they run over. Which pixels and texels a command ends up
// with is decided by how those counters are set up and by their width, so every one here has to count exactly
// as the chip's does; the software rasteriser (raster.cpp) is compared pixel for pixel with the reference build.
//
// Written for satrecomp. The rules themselves (the starting values, which side a tie falls to, the 13-bit
// width) are the hardware's, as worked out by the Saturn emulation community and learnt here from the
// Ymir emulator's source (https://github.com/StrikerX3/Ymir), which this code's results were checked against.
#pragma once

#include "base.h"

#include <algorithm>
#include <cstdlib>

namespace satrecomp::video {

// The edge and line counters are 13 bits wide and signed: sums wrap round at 4096
constexpr sint32 Wrap13(sint32 value) {
    return static_cast<sint32>(((static_cast<uint32>(value) + 0x1000u) & 0x1FFFu)) - 0x1000;
}

constexpr sint32 Direction(sint32 delta) {
    return delta < 0 ? -1 : 1;
}

// Which texel of a row (or which row of a character) each pixel of a line shows, going from `first` to `last`
// over `pixels` pixels. Before a pixel is drawn the walk is moved on for as long as it is Behind(); after it,
// PixelDone().
class TexelWalk {
public:
    // everyOther: high speed shrink, where the chip looks at every second texel only, the even or the odd ones
    // (phase 0 or 1)
    void Begin(uint32 pixels, sint32 first, sint32 last, bool everyOther = false, sint32 phase = 0) {
        if (everyOther) {
            first >>= 1;
            last >>= 1;
        }
        const sint32 span = std::abs(last - first);
        const sint32 count = static_cast<sint32>(pixels);
        const bool rising = last >= first;
        m_at = everyOther ? (first << 1) | phase : first;
        m_stride = Direction(last - first) * (everyOther ? 2 : 1);
        if (count <= span) {
            // Shrinking: more texels than pixels, so some are passed over
            m_perPixel = 2 * (span + 1);
            m_perTexel = 2 * count;
            m_owed = span - 2 * count + (rising ? 1 : 0);
        } else {
            // Stretching: a texel lasts for several pixels
            m_perPixel = 2 * span;
            m_perTexel = 2 * (count - 1);
            m_owed = -count + (rising ? 0 : 1);
        }
    }
    uint32 At() const {
        return static_cast<uint32>(m_at);
    }
    bool Behind() const {
        return m_owed >= 0;
    }
    void Advance() {
        m_at += m_stride;
        m_owed -= m_perTexel;
    }
    void PixelDone() {
        m_owed += m_perPixel;
    }
    // (for pixels of the line left out by LineWalk::Clip)
    void SkipPixels(uint32 count) {
        m_owed += m_perPixel * static_cast<sint32>(count);
    }

private:
    sint32 m_at = 0, m_stride = 0;
    sint32 m_owed = 0, m_perPixel = 0, m_perTexel = 0;
};

// One colour channel of Gouraud shading going from one value to another over `pixels` pixels: so much every
// pixel, and one more whenever the remainder has added up
class Ramp {
public:
    void Begin(uint32 pixels, sint32 from, sint32 to) {
        const sint32 span = std::abs(to - from);
        const sint32 count = static_cast<sint32>(pixels);
        const bool rising = to >= from;
        m_unit = Direction(to - from);
        m_now = from;
        sint32 perPixel, start;
        if (count <= span) {
            perPixel = 2 * (span + 1);
            m_full = 2 * count;
            start = span - 2 * count + (rising ? 1 : 0);
        } else {
            perPixel = 2 * span;
            m_full = 2 * (count - 1);
            start = -count + (rising ? 0 : 1);
        }
        m_whole = 0;
        m_part = perPixel;
        if (m_full != 0) {
            if (start >= 0) {
                // (what the first pixel is already owed)
                const sint32 units = start / m_full + 1;
                m_now += units * m_unit;
                start -= units * m_full;
            }
            m_whole = (perPixel / m_full) * m_unit;
            m_part = perPixel % m_full;
        }
        m_left = -start - 1;
    }
    void Step() {
        m_now += m_whole;
        m_left -= m_part;
        if (m_left < 0) {
            m_now += m_unit;
            m_left += m_full;
        }
    }
    void Skip(uint32 pixels) {
        const sint32 count = static_cast<sint32>(pixels);
        m_now += m_whole * count;
        m_left -= m_part * count;
        if (m_full != 0 && m_left < 0) {
            const sint32 units = (-m_left + m_full - 1) / m_full;
            m_now += units * m_unit;
            m_left += units * m_full;
        }
    }
    uint8 Now() const {
        return static_cast<uint8>(m_now);
    }

private:
    sint32 m_now = 0, m_unit = 1;
    sint32 m_whole = 0; // added every pixel
    sint32 m_part = 0;  // taken from m_left every pixel; below nothing, one unit more and m_full back
    sint32 m_full = 0, m_left = 0;
};

// Gouraud shading along a line or an edge: a ramp for each of red, green and blue
class Gradient {
public:
    void Begin(uint32 pixels, Color555 from, Color555 to) {
        m_red.Begin(pixels, from.r, to.r);
        m_green.Begin(pixels, from.g, to.g);
        m_blue.Begin(pixels, from.b, to.b);
    }
    void Step() {
        m_red.Step();
        m_green.Step();
        m_blue.Step();
    }
    void Skip(uint32 pixels) {
        if (pixels != 0) {
            m_red.Skip(pixels);
            m_green.Skip(pixels);
            m_blue.Skip(pixels);
        }
    }
    Color555 Now() const {
        return Color555{.r = m_red.Now(), .g = m_green.Now(), .b = m_blue.Now()};
    }

private:
    Ramp m_red, m_green, m_blue;
};

// The pixels of a line from one point to another, one for each step along its longer axis. With `fill` (the
// lines a quad is made of) a step that also moves along the shorter axis has a second pixel, Beside(), so that
// neighbouring lines leave no holes.
//
//     LineWalk line{from, to, fill};
//     bool second = false; // (the pixel a line starts on never has one)
//     for (line.Move(); line.More(); second = line.Move()) { ... line.At() ... }
class LineWalk {
public:
    LineWalk(CoordS32 from, CoordS32 to, bool fill) : m_at(from), m_fill(fill) {
        const sint32 dx = to.x() - from.x(), dy = to.y() - from.y();
        const sint32 lengthX = std::abs(dx), lengthY = std::abs(dy);
        const bool wide = lengthX >= lengthY;
        const sint32 along = wide ? dx : dy;
        const sint32 longer = wide ? lengthX : lengthY, shorter = wide ? lengthY : lengthX;
        (wide ? m_long.x() : m_long.y()) = Direction(along);
        (wide ? m_short.y() : m_short.x()) = Direction(wide ? dy : dx);
        m_to = to;
        m_span = static_cast<uint32>(longer);
        m_pixels = m_span + 1;

        m_fall = Wrap13(2 * shorter);
        m_refill = Wrap13(2 * longer);
        // An outline's tie goes to one side whichever end it is drawn from; a fill's starts one lower
        m_floor = fill ? -1 : (along < 0 ? 1 : 0);
        m_level = Wrap13(longer + 1 + 2 * shorter - (fill ? 1 : 0));

        if (fill) {
            // The second pixel is the one that keeps the line joined to the next: back along one axis or the other
            const bool sameWay = (from.x() > to.x()) == (from.y() > to.y());
            if (sameWay) {
                m_beside.y() = -(wide ? m_short.y() : m_long.y());
            } else {
                m_beside.x() = -(wide ? m_long.x() : m_short.x());
            }
        }
        // (the first Move() arrives at `from`)
        m_at.x() -= m_long.x();
        m_at.y() -= m_long.y();
    }

    // Leaves out what is certain to be outside a drawing area of width by height (to their last coordinates).
    // Returns how many pixels were left out at the start, which texture and shading have to skip too.
    uint32 Clip(sint32 width, sint32 height) {
        const sint32 lowest = -1, highestX = width + 2, highestY = height + 2; // (with a margin)
        if (m_span == 0) {
            return 0;
        }
        auto inX = [&](sint32 x) { return x >= lowest && x <= highestX; };
        auto inY = [&](sint32 y) { return y >= lowest && y <= highestY; };
        if (inX(m_at.x()) && inX(m_to.x()) && inY(m_at.y()) && inY(m_to.y())) {
            return 0;
        }
        if ((m_at.x() < lowest && m_to.x() < lowest) || (m_at.x() > highestX && m_to.x() > highestX) ||
            (m_at.y() < lowest && m_to.y() < lowest) || (m_at.y() > highestY && m_to.y() > highestY)) {
            m_pixels = 0; // nothing of it is inside
            return 0;
        }

        const bool wide = m_long.x() != 0;
        const sint32 way = wide ? m_long.x() : m_long.y();
        const sint32 head = wide ? m_at.x() : m_at.y(), tail = wide ? m_to.x() : m_to.y();
        const sint32 highest = wide ? highestX : highestY;
        uint32 left = m_span;

        uint32 before = 0;
        if (way > 0 && head < lowest) {
            before = static_cast<uint32>(lowest - 1 - head);
        } else if (way < 0 && head > highest) {
            before = static_cast<uint32>(head - highest);
        }
        if (before != 0) {
            before = std::min(before, left - 1);
            m_at.x() += m_long.x() * static_cast<sint32>(before);
            m_at.y() += m_long.y() * static_cast<sint32>(before);
            // (A refill of half the counter's range, which a line 2048 long has, brings the level back to where
            // it was after two: such a line could go round for ever here, so it gets one refill a step, as
            // Move() gives every line. No other length can.)
            const bool once = m_refill == -0x1000;
            for (uint32 i = 0; i < before; i++) {
                m_level = Wrap13(m_level - m_fall);
                while (m_refill != 0 && m_level <= m_floor) {
                    m_level = Wrap13(m_level + m_refill);
                    m_at.x() += m_short.x();
                    m_at.y() += m_short.y();
                    if (once) {
                        break;
                    }
                }
            }
            left -= before;
        }

        uint32 after = 0;
        if (way < 0 && tail < lowest) {
            after = static_cast<uint32>(lowest - 1 - tail);
        } else if (way > 0 && tail > highest) {
            after = static_cast<uint32>(tail - highest);
        }
        after = std::min(after, left);
        if (after != 0) {
            left -= after;
        }
        m_pixels = left + 1;
        return before;
    }

    // Goes to the next pixel. True if it has a second pixel beside it (fills only).
    bool Move() {
        ++m_moves;
        m_at.x() += m_long.x();
        m_at.y() += m_long.y();
        m_level = Wrap13(m_level - m_fall);
        if (m_level <= m_floor) {
            m_level = Wrap13(m_level + m_refill);
            m_at.x() += m_short.x();
            m_at.y() += m_short.y();
            return m_fill;
        }
        return false;
    }
    bool More() const {
        return m_moves <= m_pixels;
    }
    CoordS32 At() const {
        return m_at;
    }
    CoordS32 Beside() const {
        return {m_at.x() + m_beside.x(), m_at.y() + m_beside.y()};
    }
    // The line's length along its longer axis, as it was given (texture and shading are spread over this + 1)
    uint32 Span() const {
        return m_span;
    }

private:
    CoordS32 m_at, m_to{};
    CoordS32 m_long{}, m_short{}; // one step along the longer axis; along the shorter, when the counter says
    CoordS32 m_beside{};
    sint32 m_level = 0, m_fall = 0, m_refill = 0, m_floor = 0;
    uint32 m_span = 0, m_pixels = 0, m_moves = 0;
    bool m_fill;
};

// A 13-bit counter that is added to and, on reaching its mark, gives a pulse and has an amount taken off
struct Pulse13 {
    sint32 sum = 0, gain = 0, cost = 0, mark = 0;
    void Set(sint32 start, sint32 perStep, sint32 perPulse, sint32 at) {
        sum = Wrap13(start);
        gain = Wrap13(perStep);
        cost = Wrap13(perPulse);
        mark = at;
    }
    bool Add() {
        sum = Wrap13(sum + gain);
        if (sum < mark) {
            return false;
        }
        sum = Wrap13(sum - cost);
        return true;
    }
};

// One side of the ladder a quad is drawn as: the point a rung starts or ends at, moving from one corner to the
// next in `rungs` steps. The side moves a pixel at a time along its own length, but only on the steps its
// length's share of the rungs comes to.
class Rail {
public:
    void Begin(CoordS32 from, CoordS32 to, uint32 rungs) {
        const sint32 dx = bit::sign_extend<13>(to.x() - from.x()), dy = bit::sign_extend<13>(to.y() - from.y());
        const sint32 lengthX = std::abs(dx), lengthY = std::abs(dy);
        const sint32 length = std::max(lengthX, lengthY);
        m_at = from;
        m_way = {Direction(dx), Direction(dy)};
        m_length = static_cast<uint32>(length);
        // (ties fall the other way when the side runs backwards along the other axis)
        const sint32 markX = dy < 0 ? -1 : 0, markY = dx < 0 ? -1 : 0;
        m_x.Set(-length - 1, 2 * lengthX, 2 * length, markX);
        m_y.Set(-length - 1, 2 * lengthY, 2 * length, markY);
        const sint32 steps = static_cast<sint32>(rungs);
        m_move.Set(-steps - 1, 2 * length, 2 * steps, lengthX >= lengthY ? markY : markX);
        m_shaded = false;
    }
    void BeginShade(Color555 from, Color555 to) {
        m_shade.Begin(m_length + 1, from, to);
        m_shaded = true;
    }
    void Step() {
        if (!m_move.Add()) {
            return;
        }
        if (m_x.Add()) {
            m_at.x() += m_way.x();
        }
        if (m_y.Add()) {
            m_at.y() += m_way.y();
        }
        if (m_shaded) {
            m_shade.Step();
        }
    }
    CoordS32 At() const {
        return m_at;
    }
    Color555 ShadeNow() const {
        return m_shade.Now();
    }

private:
    CoordS32 m_at{}, m_way{};
    Pulse13 m_move, m_x, m_y;
    Gradient m_shade;
    uint32 m_length = 0;
    bool m_shaded = false;
};

// A quad with corners A, B, C, D in order round it, drawn as lines ("rungs") from the side A-D to the side B-C:
//
//     A --- B        as many rungs as the longer of the two sides has pixels, so that the longer side
//     |     |        moves a pixel every rung and the other keeps pace with it
//     D --- C
class Ladder {
public:
    Ladder(CoordS32 a, CoordS32 b, CoordS32 c, CoordS32 d) {
        auto reach = [](CoordS32 from, CoordS32 to) {
            return static_cast<uint32>(std::max(std::abs(bit::sign_extend<13>(to.x() - from.x())),
                                                std::abs(bit::sign_extend<13>(to.y() - from.y()))));
        };
        m_rungs = std::max(reach(a, d), reach(b, c)) & 0xFFF;
        m_left.Begin(a, d, m_rungs);
        m_right.Begin(b, c, m_rungs);

        // Which way the outline turns at each corner (as the screen has it, y downwards: more than nothing is
        // clockwise). The rungs of a quad that turns clockwise at all four can leave the drawing area and come
        // back into it, and the rasteriser gives such a quad a second run; any other kind gets one. This is the
        // rule the reference's pictures are matched with, kept as it is found there.
        auto turn = [](CoordS32 p, CoordS32 q, CoordS32 r) {
            return (q.x() - p.x()) * (r.y() - q.y()) - (q.y() - p.y()) * (r.x() - q.x());
        };
        m_twoRuns = turn(a, b, c) > 0 && turn(b, c, d) > 0 && turn(c, d, a) > 0 && turn(d, a, b) > 0;
    }
    // The rows of a character `height` texels high, one for each rung
    void BeginRows(TexelWalk &rows, uint32 height, bool upsideDown) const {
        const sint32 last = static_cast<sint32>(height) - 1;
        rows.Begin(m_rungs + 1, upsideDown ? last : 0, upsideDown ? 0 : last);
    }
    void BeginShade(Color555 a, Color555 b, Color555 c, Color555 d) {
        m_left.BeginShade(a, d);
        m_right.BeginShade(b, c);
    }
    bool More() const {
        return m_done <= m_rungs;
    }
    void Step() {
        ++m_done;
        m_left.Step();
        m_right.Step();
    }
    const Rail &Left() const {
        return m_left;
    }
    const Rail &Right() const {
        return m_right;
    }
    bool TwoRuns() const {
        return m_twoRuns;
    }

private:
    Rail m_left, m_right;
    uint32 m_rungs = 0, m_done = 0;
    bool m_twoRuns = false;
};

} // namespace satrecomp::video
