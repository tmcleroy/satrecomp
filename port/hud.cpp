// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The readout over the picture (hud.h).
#include "hud.h"

#include "game.h"
#include "../recomp-common/text.h"

#include <algorithm>
#include <cmath>

namespace port {

namespace {
    std::string g_note;
    uint64_t g_noteSerial = 0;

    struct Rgba {
        uint8_t r, g, b, a;
    };
    constexpr Rgba kPanel{20, 22, 30, 215}, kWhite{255, 255, 255, 255};

    // A picture drawn on with alpha (what is under may itself be see-through)
    struct Layer {
        uint32_t *pixels;
        int width, height;

        void Put(int x, int y, Rgba c) const {
            uint32_t &pixel = pixels[static_cast<size_t>(y) * width + x];
            const uint32_t underAlpha = pixel >> 24, alpha = c.a + underAlpha * (255 - c.a) / 255;
            if (alpha == 0) {
                return;
            }
            const auto mix = [&](uint32_t under, uint32_t over) { return (over * c.a + under * underAlpha * (255 - c.a) / 255) / alpha; };
            pixel = (alpha << 24) | (mix((pixel >> 16) & 0xFF, c.b) << 16) | (mix((pixel >> 8) & 0xFF, c.g) << 8) | mix(pixel & 0xFF, c.r);
        }
        void Rect(float x, float y, float w, float h, Rgba c) const {
            const int x0 = std::max(0, static_cast<int>(x)), y0 = std::max(0, static_cast<int>(y));
            const int x1 = std::min(width, static_cast<int>(x + w)), y1 = std::min(height, static_cast<int>(y + h));
            for (int py = y0; py < y1; py++) {
                for (int px = x0; px < x1; px++) {
                    Put(px, py, c);
                }
            }
        }
        void Text(float x, float y, float dot, Rgba c, const std::string &s) const {
            for (size_t i = 0; i < s.size(); i++, x += recomp::text::kCharWidth * dot) {
                unsigned ch = static_cast<unsigned char>(s[i]);
                if (ch < 32 || ch > 126) {
                    ch = '?';
                }
                const uint8_t *glyph = recomp::text::kFont[ch - 32];
                for (int column = 0; column < 5; column++) {
                    for (int row = 0; row < 8; row++) {
                        if (glyph[column] & (1 << row)) {
                            Rect(x + column * dot, y + row * dot, dot, dot, c);
                        }
                    }
                }
            }
        }
        // (clipped to the box given, the panel a diagram's circle is on)
        void Circle(float cx, float cy, float radius, float line, bool filled, Rgba c, int left, int top, int right, int bottom) const {
            const int x0 = std::max(static_cast<int>(cx - radius - 1), left), x1 = std::min(static_cast<int>(cx + radius + 2), right);
            const int y0 = std::max(static_cast<int>(cy - radius - 1), top), y1 = std::min(static_cast<int>(cy + radius + 2), bottom);
            for (int py = y0; py < y1; py++) {
                for (int px = x0; px < x1; px++) {
                    const float dx = static_cast<float>(px) + 0.5f - cx, dy = static_cast<float>(py) + 0.5f - cy;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance <= radius && (filled || distance >= radius - line)) {
                        Put(px, py, c);
                    }
                }
            }
        }
    };
} // namespace

void SetHudNote(const std::string &text) {
    if (text != g_note) {
        g_note = text;
        g_noteSerial++;
    }
}
uint64_t HudShownSerial() {
    return satrecomp::HudSerial() + g_noteSerial;
}
bool HudShows() {
    return !g_note.empty() || !satrecomp::Hud().empty() || !satrecomp::HudCircles().empty() || satrecomp::TheHudDiagram().width > 0;
}

void DrawHud(uint32_t *pixels, int width, int height) {
    const Layer layer{pixels, width, height};
    // A font dot is a pixel of a 240-line picture
    const float d = std::max(static_cast<float>(height) / 240.0f, 1.0f);
    const float thin = std::max(d * 0.6f, 1.0f); // (an outline's width)

    // Circles over the picture (see-through where filled, so that what they lie over still shows)
    for (const satrecomp::HudCircle &circle : satrecomp::HudCircles()) {
        layer.Circle(circle.x * static_cast<float>(width), circle.y * static_cast<float>(height), std::max(circle.radius * static_cast<float>(height), 1.5f), thin,
                     circle.filled, {circle.r, circle.g, circle.b, static_cast<uint8_t>(circle.filled ? 120 : 255)}, 0, 0, width, height);
    }

    // The diagram, on its panel
    const satrecomp::HudDiagram &diagram = satrecomp::TheHudDiagram();
    const int diagramWidth = static_cast<int>(diagram.width * static_cast<float>(height)), diagramHeight = static_cast<int>(diagram.height * static_cast<float>(height));
    if (diagramWidth > 0 && diagramHeight > 0 && diagramWidth <= width && diagramHeight <= height) {
        const int at = static_cast<int>(diagram.x * static_cast<float>(width));
        const int left = std::clamp(diagram.align < 0 ? at : diagram.align > 0 ? at - diagramWidth : at - diagramWidth / 2, 0, width - diagramWidth);
        const int top = std::clamp(static_cast<int>(diagram.y * static_cast<float>(height)), 0, height - diagramHeight);
        layer.Rect(static_cast<float>(left), static_cast<float>(top), static_cast<float>(diagramWidth), static_cast<float>(diagramHeight), kPanel);
        const float sx = static_cast<float>(diagramWidth) / diagram.unitsWide, sy = static_cast<float>(diagramHeight) / diagram.unitsHigh;
        for (const satrecomp::HudCircle &circle : diagram.circles) {
            layer.Circle(static_cast<float>(left) + circle.x * sx, static_cast<float>(top + diagramHeight) - circle.y * sy, std::max(circle.radius * sx, 1.0f), thin, circle.filled,
                         {circle.r, circle.g, circle.b, 255}, left, top, left + diagramWidth, top + diagramHeight);
        }
    }

    // The lines, each on a panel of its own
    std::vector<satrecomp::HudLine> lines = satrecomp::Hud();
    if (!g_note.empty()) {
        lines.push_back({0.5f, 0.02f, 0, g_note});
    }
    const float pad = 3 * d;
    for (const satrecomp::HudLine &line : lines) {
        if (line.text.empty()) {
            continue;
        }
        const float w = recomp::text::TextWidth(line.text.c_str(), d) + 2 * pad, h = 8 * d + 2 * pad;
        const float at = line.x * static_cast<float>(width);
        const float x = std::clamp(line.align < 0 ? at : line.align > 0 ? at - w : at - w / 2, 0.0f, std::max(static_cast<float>(width) - w, 0.0f));
        const float y = std::clamp(line.y * static_cast<float>(height), 0.0f, std::max(static_cast<float>(height) - h, 0.0f));
        layer.Rect(x, y, w, h, kPanel);
        layer.Text(x + pad, y + pad, d, kWhite, line.text);
    }
}

} // namespace port
