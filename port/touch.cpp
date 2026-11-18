// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
//
// The controls on a touch screen (touch.h).
#include "touch.h"

#include "bindings.h"
#include "port.h"
#include "../recomp-common/menu.h"
#include "../recomp-common/text.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#ifndef SATRECOMP_TOUCH
#define SATRECOMP_TOUCH ""
#endif

namespace port {

namespace {

constexpr int kLeast = 4; // the frames a press lasts at least (a quick tap's down and up can come in one frame)
constexpr uint64_t kIdleMs = 5000; // no finger for this long: not drawn

// A colour, its alpha premultiplied, as the layer takes it (red in the low byte)
uint32_t Premultiplied(int r, int g, int b, float alpha) {
    const auto by = [alpha](int c) { return static_cast<uint32_t>(std::lround(c * alpha)); };
    return by(r) | by(g) << 8 | by(b) << 16 | static_cast<uint32_t>(std::lround(255 * alpha)) << 24;
}

// One colour over another, both premultiplied
uint32_t Over(uint32_t top, uint32_t under) {
    const uint32_t keep = 255 - (top >> 24);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const uint32_t c = ((top >> shift) & 0xFF) + ((under >> shift) & 0xFF) * keep / 255;
        out |= std::min<uint32_t>(c, 255) << shift;
    }
    return out;
}

struct Picture {
    std::vector<uint32_t> &pixels;
    int width, height;

    // A disc, its edge smoothed; `ring`: only that much of it inside the edge (0: filled)
    void Disc(float cx, float cy, float radius, float ring, int r, int g, int b, float alpha) {
        const int x0 = std::max(0, static_cast<int>(cx - radius - 1)), x1 = std::min(width, static_cast<int>(cx + radius + 2));
        const int y0 = std::max(0, static_cast<int>(cy - radius - 1)), y1 = std::min(height, static_cast<int>(cy + radius + 2));
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                float cover = std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
                if (ring > 0) {
                    cover *= std::clamp(d - (radius - ring) + 0.5f, 0.0f, 1.0f);
                }
                if (cover > 0) {
                    uint32_t &at = pixels[static_cast<size_t>(y) * width + x];
                    at = Over(Premultiplied(r, g, b, alpha * cover), at);
                }
            }
        }
    }
    // Text in the toolkit's small font, its middle at (cx, cy)
    void Text(float cx, float cy, float dot, const std::string &text, float alpha) {
        using namespace recomp::text;
        const float w = TextWidth(text.c_str(), dot) - dot, h = 7 * dot;
        float x = cx - w / 2;
        const float y = cy - h / 2;
        const uint32_t colour = Premultiplied(255, 255, 255, alpha);
        for (unsigned char ch : text) {
            const uint8_t *glyph = kFont[(ch < 32 || ch > 126 ? '?' : ch) - 32];
            for (int col = 0; col < 5; col++) {
                for (int row = 0; row < 8; row++) {
                    if (glyph[col] & (1 << row)) {
                        for (int py = static_cast<int>(y + row * dot); py < static_cast<int>(y + (row + 1) * dot); py++) {
                            for (int px = static_cast<int>(x + col * dot); px < static_cast<int>(x + (col + 1) * dot); px++) {
                                if (px >= 0 && py >= 0 && px < width && py < height) {
                                    uint32_t &at = pixels[static_cast<size_t>(py) * width + px];
                                    at = Over(colour, at);
                                }
                            }
                        }
                    }
                }
            }
            x += kCharWidth * dot;
        }
    }
};

} // namespace

TouchControls::TouchControls() {
#ifdef __ANDROID__
    m_shown = !SDL_HasGamepad(); // (a phone's own controls, unless a gamepad is there from the start)
    m_touched = SDL_GetTicks();
#endif
    // The game's buttons, if it names them (game.cfg's touch_buttons), at most six
    for (const ComboAction &button : ParseCombos(SATRECOMP_TOUCH)) {
        std::string label = button.title;
        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (m_buttons.size() < 6) {
            m_buttons.push_back({button.bits, label.substr(0, 6)});
        }
    }
}

void TouchControls::Layout(int width, int height, const SDL_FRect &picture) {
    if (width == m_width && height == m_height && picture.x == m_picture.x && picture.w == m_picture.w) {
        return;
    }
    m_width = width;
    m_height = height;
    m_picture = picture;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    // Each side: the room beside the picture, or a fifth of the window if there is less (then over the picture)
    const float side = std::max(picture.x, w * 0.2f);
    m_stickRadius = std::min(side * 0.4f, h * 0.2f);
    m_stickX = side * 0.5f;
    m_stickY = h * 0.66f;
    m_knobX = m_stickX;
    m_knobY = m_stickY;
    m_controls.clear();
    const float button = std::min(side * 0.14f, h * 0.085f);
    const float across = w - side * 0.5f, gap = button * 2.35f;
    // (the Saturn's own: X Y Z over A B C, as on its pad)
    static const Button kSaturn[] = {{kPadX, "X"}, {kPadY, "Y"}, {kPadZ, "Z"}, {kPadA, "A"}, {kPadB, "B"}, {kPadC, "C"}};
    const std::vector<Button> buttons = m_buttons.empty() ? std::vector<Button>(std::begin(kSaturn), std::end(kSaturn)) : m_buttons;
    const int count = static_cast<int>(buttons.size());
    for (int i = 0; i < count; i++) {
        float x, y;
        int colour = i % 3;
        if (count > 3) {
            // (two rows of three; the game's own: its first three below, the rest above them)
            const int row = m_buttons.empty() ? i / 3 : 1 - i / 3, column = i % 3;
            x = across + (column - 1) * gap;
            y = h * 0.56f + row * gap - (column == 0 ? button * 0.35f : column == 2 ? -button * 0.35f : 0);
            colour = m_buttons.empty() ? column : i < 3 ? column : -1;
        } else {
            x = across + (i - (count - 1) * 0.5f) * gap;
            y = h * 0.66f;
        }
        Control c{x, y, button, buttons[static_cast<size_t>(i)].bits, buttons[static_cast<size_t>(i)].label};
        c.colour = colour;
        m_controls.push_back(c);
    }
    const float small = std::min(side * 0.12f, h * 0.06f), top = small * 1.6f;
    Control menu{side * 0.25f, top, small, 0, "MENU"};
    menu.menu = true;
    m_controls.push_back(menu);
    if (m_buttons.empty()) {
        // (the shoulder buttons, at the top of their sides)
        m_controls.push_back({side * 0.75f, top, small, kPadL, "L"});
        m_controls.push_back({w - side * 0.75f, top, small, kPadR, "R"});
    }
    m_controls.push_back({w - side * 0.25f, top, small, kPadStart, "START"});
    Changed();
}

int TouchControls::Hit(float x, float y) const {
    // (the nearest whose disc, a little larger than drawn, is under the finger)
    int found = -1;
    float best = 0;
    for (size_t i = 0; i < m_controls.size(); i++) {
        const Control &c = m_controls[i];
        const float d = std::hypot(x - c.x, y - c.y) / (c.radius * 1.3f);
        if (d < 1 && (found < 0 || d < best)) {
            found = static_cast<int>(i);
            best = d;
        }
    }
    return found;
}

bool TouchControls::Event(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        // (a screen's fingers; not a laptop's trackpad, whose touches are the mouse's)
        if (SDL_GetTouchDeviceType(event.tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT) {
            return false;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_KEY_DOWN:
        // (a gamepad or the keys are being played with; not the system's Back, which a phone has too)
        if (m_shown && !(event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_AC_BACK)) {
            m_shown = false;
            m_fingers.clear();
            m_lingering.clear();
            m_stick = 0;
            Changed();
        }
        return false;
    default: return false;
    }
    m_touched = SDL_GetTicks();
    if (!m_shown || m_idle) {
        m_shown = true;
        m_idle = false;
        Changed();
    }
    const float x = event.tfinger.x * m_width, y = event.tfinger.y * m_height;
    const SDL_FingerID finger = event.tfinger.fingerID;
    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
        if (auto it = m_fingers.find(finger); it != m_fingers.end()) {
            if (it->second.control < 0) {
                if (it->second.reads < kLeast && m_stick) {
                    m_lingering.push_back({m_stick, kLeast - it->second.reads});
                }
                m_stick = 0;
                m_knobX = m_stickX;
                m_knobY = m_stickY;
            } else if (it->second.reads < kLeast) {
                m_lingering.push_back({m_controls[static_cast<size_t>(it->second.control)].bits, kLeast - it->second.reads});
            }
            m_fingers.erase(it);
            Changed();
        }
        return true;
    }
    auto it = m_fingers.find(finger);
    if (event.type == SDL_EVENT_FINGER_DOWN) {
        // The stick takes a finger anywhere in the left part away from the buttons at the top
        const int hit = Hit(x, y);
        const bool stick = hit < 0 && x < std::max(m_picture.x, m_width * 0.2f) * 1.15f;
        if (hit < 0 && !stick) {
            return true;
        }
        if (hit >= 0 && m_controls[static_cast<size_t>(hit)].menu) {
            m_menuPressed = true;
        }
        it = m_fingers.emplace(finger, Finger{stick ? -1 : hit, 0}).first;
    } else if (it == m_fingers.end()) {
        return true;
    }
    if (it->second.control < 0) {
        // Eight ways: each a quarter of the way round from its neighbours, none within a fifth of the reach
        const float dx = x - m_stickX, dy = y - m_stickY, d = std::hypot(dx, dy);
        const float reach = std::min(d, m_stickRadius);
        m_knobX = d > 0 ? m_stickX + dx / d * reach : m_stickX;
        m_knobY = d > 0 ? m_stickY + dy / d * reach : m_stickY;
        uint16_t now = 0;
        if (d > m_stickRadius * 0.2f) {
            const float angle = std::atan2(-dy, dx) * 180.0f / 3.14159265f; // (0 right, 90 up)
            const int way = static_cast<int>(std::floor((angle + 360.0f + 22.5f) / 45.0f)) % 8;
            static const uint16_t kWays[8] = {kPadRight, kPadUp | kPadRight, kPadUp, kPadUp | kPadLeft,
                                              kPadLeft,  kPadDown | kPadLeft, kPadDown, kPadDown | kPadRight};
            now = kWays[way];
        }
        m_stick = now;
        Changed();
    } else if (event.type == SDL_EVENT_FINGER_MOTION) {
        // (a finger slid from one button onto another presses that one instead, as on a pad)
        const int hit = Hit(x, y);
        // (not onto or off the menu's: that is pressed once, where the finger goes down)
        const auto own = [&](int control) { return m_controls[static_cast<size_t>(control)].menu; };
        if (hit >= 0 && hit != it->second.control && !own(hit) && !own(it->second.control)) {
            it->second = {hit, 0};
        }
    }
    Changed();
    return true;
}

uint16_t TouchControls::Read(bool menuOpen) {
    m_stickHeld = m_stick && m_stick == m_stickBefore ? m_stickHeld + 1 : 0;
    m_stickBefore = m_stick;
    // (held past a moment in the menu, let go for a frame in every four: another press of it, on to the next row)
    uint16_t bits = menuOpen && m_stickHeld > 18 && m_stickHeld % 4 == 0 ? 0 : m_stick;
    for (auto &[finger, held] : m_fingers) {
        if (held.control >= 0) {
            bits |= m_controls[static_cast<size_t>(held.control)].bits;
        }
        held.reads++;
    }
    for (auto &[lingering, left] : m_lingering) {
        bits |= lingering;
        left--;
    }
    m_lingering.erase(std::remove_if(m_lingering.begin(), m_lingering.end(), [](const auto &l) { return l.second <= 0; }), m_lingering.end());
    m_read = m_shown ? bits : 0;
    return m_read;
}

unsigned TouchControls::Menu() const {
    // (the first button chooses, the second goes back: on the pad's own layout A and B)
    uint16_t choose = kPadA, back = kPadB;
    if (!m_buttons.empty()) {
        choose = m_buttons[0].bits;
        back = m_buttons.size() > 1 ? m_buttons[1].bits : 0;
    }
    unsigned menu = 0;
    menu |=(m_read & choose) == choose && choose ? recomp::kMenuConfirm : 0;
    menu |= back && (m_read & back) == back && (m_read & choose) != choose ? recomp::kMenuBack : 0;
    return menu;
}

bool TouchControls::TakeMenuPress() {
    const bool pressed = m_menuPressed;
    m_menuPressed = false;
    return pressed;
}

bool TouchControls::TakePicture(std::vector<uint32_t> &pixels, int &width, int &height) {
    if (m_shown && !m_idle && m_fingers.empty() && SDL_GetTicks() - m_touched > kIdleMs) {
        m_idle = true;
        Changed();
    }
    if (!m_changed) {
        return false;
    }
    m_changed = false;
    if (!m_shown || m_idle || m_width <= 0 || m_height <= 0) {
        pixels.clear();
        return true;
    }
    Draw();
    pixels = m_pixels;
    width = m_pictureWidth;
    height = m_pictureHeight;
    return true;
}

void TouchControls::Draw() {
    // At half the window's size (stretched to it, smoothly)
    const float by = 0.5f;
    m_pictureWidth = std::max(1, m_width / 2);
    m_pictureHeight = std::max(1, m_height / 2);
    m_pixels.assign(static_cast<size_t>(m_pictureWidth) * m_pictureHeight, 0);
    Picture p{m_pixels, m_pictureWidth, m_pictureHeight};
    const float dot = std::max(1.0f, std::round(m_height * by / 260.0f));
    // The stick: its reach, and the knob where the finger holds it
    p.Disc(m_stickX * by, m_stickY * by, m_stickRadius * by, 0, 255, 255, 255, 0.10f);
    p.Disc(m_stickX * by, m_stickY * by, m_stickRadius * by, 2 * dot, 255, 255, 255, 0.35f);
    p.Disc(m_knobX * by, m_knobY * by, m_stickRadius * 0.42f * by, 0, 255, 255, 255, m_stick ? 0.55f : 0.35f);
    std::vector<bool> down(m_controls.size(), false);
    for (const auto &[finger, held] : m_fingers) {
        if (held.control >= 0) {
            down[static_cast<size_t>(held.control)] = true;
        }
    }
    // The face buttons in three colours (as the Saturn's pad has them, a column each), the rest grey
    static const int kColours[3][3] = {{70, 140, 255}, {80, 200, 110}, {240, 70, 70}};
    // (every label in one size: the size the longest of the big buttons' fits in, and the small ones' theirs)
    const auto fits = [&](const Control &c) {
        return c.label.size() > 3 ? std::max(1.0f, std::floor(c.radius * by * 1.6f / (c.label.size() * recomp::text::kCharWidth))) : dot;
    };
    float bigSize = dot, smallSize = dot;
    for (const Control &c : m_controls) {
        float &size = c.radius == m_controls.front().radius ? bigSize : smallSize;
        size = std::min(size, fits(c));
    }
    for (size_t i = 0; i < m_controls.size(); i++) {
        const Control &c = m_controls[i];
        const int *colour = c.colour >= 0 ? kColours[c.colour] : nullptr;
        const int r = colour ? colour[0] : 200, g = colour ? colour[1] : 200, b = colour ? colour[2] : 200;
        p.Disc(c.x * by, c.y * by, c.radius * by, 0, r, g, b, down[i] ? 0.65f : 0.25f);
        p.Disc(c.x * by, c.y * by, c.radius * by, 1.5f * dot, r, g, b, 0.7f);
        p.Text(c.x * by, c.y * by, c.radius == m_controls.front().radius ? bigSize : smallSize, c.label, 0.85f);
    }
}

} // namespace port
