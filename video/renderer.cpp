// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "renderer.h"

#include "../runtime/state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace satrecomp::video {

Renderer::Renderer(const Memory &memory)
    : m_mem(memory) {}

void Renderer::Reset(bool hard) {
    if (hard) {
        std::fill(m_framebuffer.begin(), m_framebuffer.end(), 0xFF000000);
    }
    m_commands.clear();
    m_lastCommands.clear();
    std::fill(std::begin(m_raw2), std::end(m_raw2), 0);
    const bool odd = m_regs2.TVSTAT.ODD;
    m_regs2 = {};
    m_regs2.TVSTAT.ODD = odd;
    m_regs2.Decode(m_raw2);
    m_regs1 = {};
    m_ctx = {};
    m_displayFB = 1;
}

void Renderer::SetMode(Mode mode, FrameCallback onFrame, Reference *reference) {
    m_compose = mode != Mode::Reference;
    m_check = mode == Mode::Check;
    m_gpu = mode == Mode::Gpu;
    m_onFrame = std::move(onFrame);
    m_ref = reference;
    if (m_compose) {
        m_framebuffer.assign(704 * 512, 0xFF000000);
    }
}

void Renderer::Serialize(Archive &ar) {
    ar.Mark(0x56494445);
    ar.Pod(m_raw2);
    ar.Pod(m_regs1);
    ar.Pod(m_regs2);
    ar.Pod(m_ctx);
    ar.Pod(m_displayFB);
    ar.Pod(m_width);
    ar.Pod(m_height);
    ar.Pod(m_exclusive);
    ar.Pod(m_backColor);
    ar.Pod(m_scroll);
    for (Rotation &rot : m_rot) {
        ar.Pod(rot.Xst);
        ar.Pod(rot.Yst);
        ar.Pod(rot.KA);
    }
    if (!ar.Saving()) {
        // (a GPU backend's copy of the VDP1's framebuffers is of before: it starts them empty)
        m_commands.clear();
        m_frame.events.clear();
        m_frame.quads.clear();
        m_frame.vram.clear();
        m_frame.resetSprites = true;
        m_drawFirstQuad = 0;
        m_drawing = false;
        m_spriteWindowImage.clear(); // (the layer images are made anew at the size the state has)
        if (m_compose) {
            m_framebuffer.resize(std::max<size_t>(m_framebuffer.size(), static_cast<size_t>(m_width) * m_height));
        }
    }
}

// --- registers ---

void Renderer::WriteVdp1Reg(uint32 address, uint16 value) {
    m_regs1.Write(address, value);
}

void Renderer::WriteVdp2Reg(uint32 address, uint16 value) {
    const uint32 index = (address & 0x1FE) >> 1;
    m_raw2[index] = value;
    m_regs2.Decode(m_raw2);
    if (index == 0x000 / 2) {
        // (a display turned off takes effect at once, one turned on with the next frame)
        m_regs2.displayEnabledLatch = m_regs2.displayEnabledLatch && m_regs2.TVMD.DISP;
    } else if (index == 0x0B2 / 2) {
        // RPRCTL: the game asks for a rotation parameter set's start values to be read again
        for (uint32 i = 0; i < 2; i++) {
            RotationParams &params = m_regs2.rotParams[i];
            params.readXst = Bit(value, i * 8);
            params.readYst = Bit(value, i * 8 + 1);
            params.readKAst = Bit(value, i * 8 + 2);
        }
    }
}

void Renderer::SetField(bool odd) {
    m_regs2.TVSTAT.ODD = odd;
}

void Renderer::LatchDisplayMode() {
    m_regs2.displayEnabledLatch = m_regs2.TVMD.DISP;
    m_regs2.borderColorModeLatch = m_regs2.TVMD.BDCLMD;
}

// --- VDP1 ---

void Renderer::EraseFramebuffer(uint64 cycles) {
    if (m_gpu) {
        Vdp1Erase();
    } else if (m_compose) {
        RasterErase(cycles);
    }
}

void Renderer::SwapFramebuffer() {
    m_regs1.LatchEraseParameters();
    if (m_gpu) {
        Vdp1Swap();
    }
    m_displayFB ^= 1;
}

void Renderer::BeginDrawing() {
    m_commands.clear();
    m_drawing = true;
    if (m_gpu) {
        Vdp1BeginDraw();
    }
}

void Renderer::DrawCommand(uint32 address) {
    Command &cmd = m_commands.emplace_back();
    cmd.address = address;
    for (uint32 i = 0; i < 16; i++) {
        cmd.words[i] = Be16(&m_mem.vram1[(address + i * 2) & 0x7FFFE]);
    }
    if (m_gpu) {
        Vdp1Command(cmd);
    } else if (m_compose) {
        RasterCommand(cmd);
    }
}

void Renderer::EndDrawing() {
    static const bool info = getenv("SATRECOMP_VDP1_INFO") != nullptr;
    static unsigned frames = 0;
    if (info && (++frames % 500) == 0) {
        const Vdp1Regs &r = m_regs1;
        const auto &c = m_ctx;
        uint64_t texBytes = 0, maxW = 0, maxH = 0;
        int minX = 9999, maxX = -9999, minY = 9999, maxY = -9999;
        for (const Command &cmd : m_commands) {
            const unsigned type = cmd.words[0] & 0xF;
            if (type < 4) {
                const unsigned w = ((cmd.words[5] >> 8) & 0x3F) * 8, h = cmd.words[5] & 0xFF;
                texBytes += w * h;
                maxW = std::max<uint64_t>(maxW, w);
                maxH = std::max<uint64_t>(maxH, h);
            }
            if (type < 8) {
                for (int i = 0; i < (type == 0 ? 1 : 4); i++) {
                    const int x = bit::sign_extend<13>(cmd.words[6 + i * 2]), y = bit::sign_extend<13>(cmd.words[7 + i * 2]);
                    minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
            }
        }
        fprintf(stderr, "vdp1: fb %ux%u 8bit %d rot %d hdtv %d DIE %d DIL %d swapmode %d vbe %d erase (%u,%u)-(%u,%u)=%04X sysclip %d,%d userclip %d,%d-%d,%d local %d,%d | %zu cmds, texels %llu, max %llux%llu, coords x %d..%d y %d..%d, vram writes while drawing %llu, vdp2 %ux%u\n",
                r.fbSizeH, r.fbSizeV, r.pixel8Bits, r.fbRotEnable, r.hdtvEnable, r.dblInterlaceEnable, r.dblInterlaceDrawLine, r.fbSwapMode,
                r.vblankErase, r.eraseX1, r.eraseY1, r.eraseX3, r.eraseY3, r.eraseWriteValue, (int)c.sysClipH, (int)c.sysClipV,
                (int)c.userClipX0, (int)c.userClipY0, (int)c.userClipX1, (int)c.userClipY1, (int)c.localCoordX, (int)c.localCoordY,
                m_commands.size(), (unsigned long long)texBytes, (unsigned long long)maxW, (unsigned long long)maxH, minX, maxX, minY, maxY,
                (unsigned long long)m_vramWritesWhileDrawing, m_width, m_height);
    }
    // (debugging aid, SATRECOMP_VDP1_DUMP=n: the nth drawing's commands that are not 3D ones)
    static const uint64_t dumpAt = getenv("SATRECOMP_VDP1_DUMP") ? strtoull(getenv("SATRECOMP_VDP1_DUMP"), nullptr, 10) : ~0ull;
    if (m_drawings == dumpAt) {
        for (const Command &cmd : m_commands) {
            const unsigned type = cmd.words[0] & 0xF;
            if (type == 2 || type == 4 || type > 7) {
                if (type > 7) {
                    fprintf(stderr, "cmd %05X type %X: %d,%d %d,%d\n", cmd.address, type, (int16_t)cmd.words[6], (int16_t)cmd.words[7], (int16_t)cmd.words[10], (int16_t)cmd.words[11]);
                }
                continue;
            }
            fprintf(stderr, "cmd %05X type %X mode %04X colr %04X src %05X size %ux%u A %d,%d B %d,%d C %d,%d D %d,%d\n", cmd.address, type, cmd.words[2],
                    cmd.words[3], cmd.words[4] * 8, ((cmd.words[5] >> 8) & 0x3F) * 8, cmd.words[5] & 0xFF, (int16_t)cmd.words[6], (int16_t)cmd.words[7],
                    (int16_t)cmd.words[8], (int16_t)cmd.words[9], (int16_t)cmd.words[10], (int16_t)cmd.words[11], (int16_t)cmd.words[12], (int16_t)cmd.words[13]);
        }
    }
    m_drawing = false;
    m_drawings++;
    m_lastCommands.swap(m_commands);
    if (m_gpu) {
        Vdp1EndDraw();
    } else if (m_check) {
        // The framebuffer drawn here against the reference's
        const uint8 *theirs = m_mem.fb[m_displayFB ^ 1u];
        const auto &mine = m_shadowFB[m_displayFB ^ 1u];
        uint64_t different = 0;
        for (size_t i = 0; i < mine.size(); i++) {
            different += mine[i] != theirs[i];
        }
        m_rasterFrames++;
        m_rasterFramesDifferent += different != 0;
        m_rasterBytesDifferent += different;
    }
}

// --- VDP2 ---

void Renderer::SetResolution(uint32 h, uint32 v, bool exclusive) {
    m_width = h;
    m_height = v;
    m_exclusive = exclusive;
    if (m_compose) {
        m_framebuffer.resize(std::max<size_t>(m_framebuffer.size(), static_cast<size_t>(h) * v));
        // A new picture size starts from an empty picture (the border colour, if that is the back screen's)
        uint32 color = 0xFF000000;
        if (m_regs2.TVMD.BDCLMD) {
            color |= m_backColor;
        }
        std::fill_n(m_framebuffer.begin(), static_cast<size_t>(h) * v, color);
    }
}

void Renderer::BeginFrame() {
    LayersBeginFrame();
}

void Renderer::RenderLine(uint32 y) {
    if (!m_compose) {
        return;
    }
    LayersBeforeLine();
    LayersPrepareLine(y);
    if (m_gpu) {
        GpuLine(y);
    } else {
        ComposeLine(y);
    }
    LayersFinishLine();
}

namespace {
    enum : uint32 { kSprites = 0, kRBG0 = 1, kBack = 6 }; // layers, in the order they win ties

    // How each of the 16 sprite data formats packs colour, priority and the shadow/window bit into a
    // framebuffer value: number of colour bits, priority field (lowest bit, number of bits), whether bit 15
    // is the shadow/window bit
    struct SpriteFormat {
        uint8 colorBits, priorityShift, priorityBits;
        bool shadowBit;
    };
    constexpr SpriteFormat kSpriteFormats[16] = {
        {11, 14, 2, false}, {11, 13, 3, false}, {11, 14, 1, true}, {11, 13, 2, true}, {10, 13, 2, true}, {11, 12, 3, true},
        {10, 12, 3, true},  {9, 12, 3, true},   {7, 7, 1, false},  {6, 7, 1, false},  {6, 6, 2, false},  {6, 0, 0, false},
        {8, 7, 1, false},   {8, 7, 1, false},   {8, 6, 2, false},  {8, 0, 0, false},
    };
} // namespace

bool Renderer::SpriteLine(uint32 y) {
    const Vdp1Regs &regs1 = m_regs1;
    const Vdp2Regs &regs2 = m_regs2;
    const SpriteParams &params = regs2.spriteParams;
    if (regs1.fbRotEnable) { // (framebuffer rotation: not handled here yet)
        if (m_check) {
            return false;
        }
        m_approximateLines[0]++;
    }
    const uint32 width = m_width;
    m_sprite.color.resize(width);
    m_sprite.priority.resize(width);
    m_sprite.special.resize(width);
    m_sprite.shadowOrWindow.resize(width);

    // The framebuffer is narrower than a high resolution picture (each pixel shows twice), or twice as wide
    // as a normal one when it holds 8-bit pixels
    const bool exclusive = (regs2.TVMD.HRESOn & 0b100) != 0;
    const bool doubleWidth = !regs1.hdtvEnable && !regs1.pixel8Bits && (regs2.TVMD.HRESOn & 0b110) == 0b010;
    const bool halfWidth = !regs1.hdtvEnable && regs1.pixel8Bits && (regs2.TVMD.HRESOn & 0b110) == 0b000;
    const uint32 outputShift = doubleWidth || exclusive ? 1 : 0;
    const uint32 readShift = halfWidth ? 1 : 0;
    const uint8 *fb = m_mem.fb[m_displayFB];
    const std::vector<uint8> &window = m_spriteWindow;
    const SpriteFormat format = kSpriteFormats[params.type & 15];
    const uint32 colorMode = regs2.vramControl.colorRAMMode;
    const auto transparent = static_cast<uint8>(SpriteSpecial::Transparent);
    const auto normal = static_cast<uint8>(SpriteSpecial::Normal);
    const auto shadow = static_cast<uint8>(SpriteSpecial::Shadow);

    for (uint32 x = 0; x < (width >> outputShift); x++) {
        const uint32 xx = x << outputShift;
        const uint32 offset = (x << readShift) + y * regs1.fbSizeH;
        uint32 color = 0;
        uint8 priority = 0, special = transparent, sow = 0;
        const uint16 word = static_cast<uint16>((fb[(offset * 2) & 0x3FFFE] << 8) | fb[((offset * 2) & 0x3FFFE) + 1]);
        if (window[xx]) {
            // (outside the sprite window: nothing)
        } else if (params.mixedFormat && (word & 0x8000) &&
                   !(params.type >= 8 ? (word & 0xFF) == 0 : params.type >= 2 && params.useSpriteWindow && (word & 0x7FFF) == 0)) {
            // An RGB pixel: its colour as it is, the first priority
            color = ConvertRGB555to888(Color555{word}).u32;
            priority = params.priorities[0];
            special = normal;
        } else if (params.mixedFormat && (word & 0x8000)) {
            // (an RGB value that stands for no pixel)
        } else {
            uint16 raw = word;
            if (regs1.pixel8Bits) {
                raw = fb[offset & 0x3FFFF];
                if (params.type < 8) {
                    raw |= 0xFF00;
                }
            }
            const uint32 colorData = raw & ((1u << format.colorBits) - 1);
            const uint32 priorityIndex = (raw >> format.priorityShift) & ((1u << format.priorityBits) - 1);
            sow = format.shadowBit && (raw & 0x8000);
            // No pixel if everything but bit 15 is zero; a shadow if the colour is all ones but the lowest bit
            special = (raw & 0x7FFF) == 0                                ? transparent
                      : colorData == (1u << format.colorBits) - 2u ? shadow
                                                                   : normal;
            if (params.useSpriteWindow && params.spriteWindowEnabled && static_cast<bool>(sow) != params.spriteWindowInverted) {
                sow = 1;
                special = transparent;
            } else {
                const uint32 index = params.colorDataOffset + colorData;
                if (colorMode == 2) {
                    color = CramLong((index * 4) & 0xFFC);
                } else {
                    color = ConvertRGB555to888(Color555{CramWord((index * 2) & (colorMode == 0 ? 0x7FE : 0xFFE))}).u32;
                }
                priority = special == transparent && !sow ? 0 : params.priorities[priorityIndex];
            }
        }
        for (uint32 i = 0; i <= (doubleWidth ? 1u : 0u); i++) {
            m_sprite.color[xx + i] = color;
            m_sprite.priority[xx + i] = priority;
            m_sprite.special[xx + i] = special;
            m_sprite.shadowOrWindow[xx + i] = sow;
        }
    }

    if (m_check) {
        m_ref->LayerLine(kSprites, width, m_refColor.data(), m_refPriority.data());
        m_ref->SpriteLine(width, m_refSpecial.data(), m_refShadowOrWindow.data());
        for (uint32 x = 0; x < width; x++) {
            const bool same = m_sprite.priority[x] == m_refPriority[x] && m_sprite.special[x] == m_refSpecial[x] &&
                              static_cast<bool>(m_sprite.shadowOrWindow[x]) == static_cast<bool>(m_refShadowOrWindow[x]) &&
                              (m_sprite.priority[x] == 0 || ((m_sprite.color[x] ^ m_refColor[x]) & 0xFFFFFF) == 0);
            m_spritePixelsDifferent += !same;
            if (!same && m_debugLeft) {
                m_debugLeft--;
                fprintf(stderr, "sprite y=%u x=%u word=%04X type=%X mixed=%d win=%d mine: pri %u special %u sow %u color %06X  ref: pri %u special %u sow %u color %06X\n",
                        y, x, static_cast<unsigned>((fb[((((x >> outputShift) << readShift) + y * regs1.fbSizeH) * 2) & 0x3FFFE] << 8) | fb[(((((x >> outputShift) << readShift) + y * regs1.fbSizeH) * 2) & 0x3FFFE) + 1]),
                        params.type, params.mixedFormat, window[x], m_sprite.priority[x], m_sprite.special[x], m_sprite.shadowOrWindow[x], m_sprite.color[x] & 0xFFFFFF,
                        m_refPriority[x], m_refSpecial[x], m_refShadowOrWindow[x], m_refColor[x] & 0xFFFFFF);
            }
        }
    }
    return true;
}

// One line of the picture from the layers' lines: for every pixel, the layer with the highest priority that
// has a pixel there (priority 0: none); between equals, sprites before RBG0 before NBG0..NBG3; the back screen
// where there is none. Then sprite shadows. Lines that need colour calculation or colour offset are left to
// the reference for now.
void Renderer::ComposeLine(uint32 y) {
    const Vdp2Regs &regs2 = m_regs2;
    const Vdp2Regs &state2 = m_regs2;
    const uint32 width = m_width;
    const uint32 line = y; // (the line of this field; y becomes the row of the picture)
    if (regs2.TVMD.IsInterlaced() && !m_exclusive) {
        y = (y << 1) | regs2.TVSTAT.ODD;
    }
    uint32 *out = &m_framebuffer[y * width];
    m_lines++;

    bool supported = !regs2.spriteParams.colorCalcEnable && !regs2.backScreenParams.colorCalcEnable;
    for (uint32 i = 0; i < 5; i++) {
        supported = supported && !regs2.bgParams[i].colorCalcEnable;
    }
    if (!supported) {
        if (m_check) {
            std::copy_n(&m_ref->Picture()[y * width], width, out);
            m_linesLeftToReference++;
            return;
        }
        m_approximateLines[6]++; // (drawn without the effect)
    }

    const Color888 back{.u32 = m_backColor};
    if (!regs2.TVMD.DISP) {
        std::fill_n(out, width, 0xFF000000 | (regs2.borderColorModeLatch ? back.u32 : 0));
        return;
    }

    SpriteWindowLine(y);
    if (!SpriteLine(line)) {
        m_sprite.color.resize(width);
        m_sprite.priority.resize(width);
        m_sprite.special.resize(width);
        m_sprite.shadowOrWindow.resize(width);
        m_ref->LayerLine(kSprites, width, m_sprite.color.data(), m_sprite.priority.data());
        m_ref->SpriteLine(width, m_sprite.special.data(), m_sprite.shadowOrWindow.data());
        m_spriteLinesLeftToReference++;
    }
    // The scroll screens' lines
    WindowLines(y);
    for (uint32 layer = kRBG0; layer <= 5u; layer++) {
        const bool rbg1 = regs2.bgEnabled[5]; // (RBG1 takes NBG0's place; not drawn here)
        if (!state2.layerEnabled[layer] || (layer == 2u && rbg1)) {
            continue;
        }
        LayerLine &mine = m_layer[layer];
        if (rbg1 && !m_check) {
            m_approximateLines[1]++;
        }
        const bool drawn = (!rbg1 || !m_check) && (layer == kRBG0 ? RotationLine() : NormalLine(layer - 2u));
        m_layerLines[layer]++;
        if (!drawn) {
            m_ref->LayerLine(layer, width, mine.color.data(), mine.priority.data());
            m_layerLinesLeftToReference[layer]++;
        } else if (m_check) {
            m_ref->LayerLine(layer, width, m_refColor.data(), m_refPriority.data());
            for (uint32 x = 0; x < width; x++) {
                const uint8 priority = m_refPriority[x];
                const bool same = mine.priority[x] == priority && (priority == 0 || ((mine.color[x] ^ m_refColor[x]) & 0xFFFFFF) == 0);
                m_layerPixelsDifferent[layer] += !same;
                if (!same && m_debugLeft) {
                    m_debugLeft--;
                    fprintf(stderr, "layer %u line %u x=%u: mine pri %u color %06X, ref pri %u color %06X\n", layer, line, x,
                            mine.priority[x], mine.color[x] & 0xFFFFFF, priority, m_refColor[x] & 0xFFFFFF);
                }
            }
        }
    }
    const LayerLine *layers = m_layer;
    const bool spriteWindow = regs2.spriteParams.useSpriteWindow;
    const auto normal = static_cast<uint8>(SpriteSpecial::Normal);
    for (uint32 x = 0; x < width; x++) {
        uint32 top = kBack;
        uint32 topPriority = 0;
        if (state2.layerEnabled[kSprites] && m_sprite.special[x] == normal) {
            // (a shadow pixel is not shown itself: it darkens what is below)
            top = m_sprite.priority[x] ? static_cast<uint32>(kSprites) : top;
            topPriority = m_sprite.priority[x];
        }
        for (uint32 layer = kRBG0; layer <= 5u; layer++) {
            if (!state2.layerEnabled[layer]) {
                continue;
            }
            const uint8 priority = layers[layer].priority[x];
            if (priority > topPriority) { // (earlier layers win ties)
                top = layer;
                topPriority = priority;
            }
        }
        Color888 color = top == kBack ? back : top == kSprites ? Color888{.u32 = m_sprite.color[x]} : Color888{.u32 = layers[top].color[x]};

        // Sprite shadow: a shadow pixel of the sprite layer halves the brightness of the layer shown, if
        // that layer takes shadows
        const bool normalShadow = m_sprite.special[x] == static_cast<uint8>(SpriteSpecial::Shadow);
        const bool msbShadow = !spriteWindow && m_sprite.shadowOrWindow[x];
        if (normalShadow || msbShadow) {
            const bool takesShadow = top == kSprites  ? static_cast<bool>(m_sprite.shadowOrWindow[x])
                                     : top == kBack ? regs2.backScreenParams.shadowEnable
                                                            : regs2.bgParams[top - kRBG0].shadowEnable;
            if (takesShadow) {
                color.u32 = (color.u32 >> 1) & 0x7F7F7F7F;
            }
        }
        // Colour offset: an amount added to (or taken from) each channel of the layer shown, for fades
        if (regs2.colorOffsetEnable[top]) {
            const auto &offset = regs2.colorOffset[regs2.colorOffsetSelect[top]];
            if (offset.nonZero) {
                auto shift = [](uint32 channel, uint32 by) { return static_cast<uint32>(std::clamp<sint32>(static_cast<sint32>(channel) + bit::sign_extend<9>(by), 0, 255)); };
                color.u32 = shift(color.r, offset.r) | (shift(color.g, offset.g) << 8) | (shift(color.b, offset.b) << 16);
            }
        }
        out[x] = color.u32;
    }
}

void Renderer::EndFrame() {
    if (!m_compose) {
        return;
    }
    if (m_gpu) {
        m_frames++;
        GpuEndFrame();
        return;
    }
    const uint32 width = m_width, height = m_height;
    m_frames++;
    if (m_check) {
        const uint32 *reference = m_ref->Picture();
        uint64_t different = 0;
        for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
            different += ((m_framebuffer[i] ^ reference[i]) & 0xFFFFFF) != 0;
        }
        m_pixelsDifferent += different;
        m_framesDifferent += different != 0;
        if (different && m_framesDifferent <= 8) {
            // (the first few differing frames: where, and what)
            size_t first = 0;
            while (((m_framebuffer[first] ^ reference[first]) & 0xFFFFFF) == 0) {
                first++;
            }
            fprintf(stderr, "video: frame %llu differs in %llu pixels (%ux%u), first at x=%zu y=%zu: %06X, reference %06X; display %d\n",
                    static_cast<unsigned long long>(m_frames), static_cast<unsigned long long>(different), width, height,
                    first % width, first / width, m_framebuffer[first] & 0xFFFFFF, reference[first] & 0xFFFFFF,
                    static_cast<int>(m_regs2.TVMD.DISP));
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
        m_framebuffer[i] |= 0xFF000000;
    }
    if (m_onFrame) {
        m_onFrame(m_framebuffer.data(), width, height);
    }
}

void Renderer::Report() const {
    if (!m_compose) {
        return;
    }
    if (!m_check) {
        fprintf(stderr, "video: %llu frames, the VDP1 drew %llu times\n", static_cast<unsigned long long>(m_frames),
                static_cast<unsigned long long>(m_drawings));
    }
    if (m_gpu) {
        if (m_gpuPixels) {
            auto percent = [&](uint64_t n) { return 100.0 * static_cast<double>(n) / static_cast<double>(m_gpuPixels); };
            fprintf(stderr, "video: GPU's VDP1 framebuffer against the reference's, of the pixels either drew: %.2f%% the same, "
                            "%.2f%% close, %.2f%% different, %.2f%% drawn only here, %.2f%% only by the reference\n",
                    percent(m_gpuSame), percent(m_gpuClose), percent(m_gpuOther), percent(m_gpuOnlyHere), percent(m_gpuOnlyReference));
        }
        if (m_gpuNotDone) {
            fprintf(stderr, "video: %llu VDP1 commands in a drawing mode not done on the GPU (shadow, half transparency, MSB on)\n",
                    static_cast<unsigned long long>(m_gpuNotDone));
        }
        return;
    }
    if (!m_check) {
        // (nothing to compare with: only what was drawn without something it asked for)
        static const char *const kWhat[7] = {"sprites (framebuffer rotation)", "RBG0", "NBG0", "NBG1", "NBG2", "NBG3",
                                             "composition (colour calculation)"};
        for (uint32 i = 0; i < 7; i++) {
            if (m_approximateLines[i]) {
                fprintf(stderr, "video: %s: %llu lines drawn without something they ask for\n", kWhat[i],
                        static_cast<unsigned long long>(m_approximateLines[i]));
            }
        }
        return;
    }
    fprintf(stderr, "video: composition: %llu frames, %llu lines, %llu left to the reference (%.2f%%)\n",
            static_cast<unsigned long long>(m_frames), static_cast<unsigned long long>(m_lines),
            static_cast<unsigned long long>(m_linesLeftToReference),
            m_lines ? 100.0 * m_linesLeftToReference / m_lines : 0.0);
    fprintf(stderr, "video: sprite layer: %llu lines left to the reference\n",
            static_cast<unsigned long long>(m_spriteLinesLeftToReference));
    static const char *const kLayerNames[6] = {"sprites", "RBG0", "NBG0", "NBG1", "NBG2", "NBG3"};
    for (uint32 layer = 1; layer < 6; layer++) {
        if (m_layerLines[layer]) {
            fprintf(stderr, "video: %s: %llu lines, %llu left to the reference, %llu pixels differ from its\n", kLayerNames[layer],
                    static_cast<unsigned long long>(m_layerLines[layer]),
                    static_cast<unsigned long long>(m_layerLinesLeftToReference[layer]),
                    static_cast<unsigned long long>(m_layerPixelsDifferent[layer]));
        }
    }
    static const char *const kParts[7] = {"sprites (framebuffer rotation)", "RBG0", "NBG0", "NBG1", "NBG2", "NBG3",
                                          "composition (colour calculation)"};
    for (uint32 i = 0; i < 7; i++) {
        if (m_approximateLines[i]) {
            fprintf(stderr, "video: %s: %llu lines drawn without something they ask for\n", kParts[i],
                    static_cast<unsigned long long>(m_approximateLines[i]));
        }
    }
    if (m_check) {
        fprintf(stderr, "video: VDP1 framebuffer against the reference's: %llu of %llu frames differ, %llu bytes in all\n",
                static_cast<unsigned long long>(m_rasterFramesDifferent), static_cast<unsigned long long>(m_rasterFrames),
                static_cast<unsigned long long>(m_rasterBytesDifferent));
        fprintf(stderr, "video: sprite layer against the reference's: %llu pixels differ\n",
                static_cast<unsigned long long>(m_spritePixelsDifferent));
        fprintf(stderr, "video: against the reference: %llu frames differ, %llu pixels in all\n",
                static_cast<unsigned long long>(m_framesDifferent), static_cast<unsigned long long>(m_pixelsDifferent));
    }
}

} // namespace satrecomp::video

namespace satrecomp::video {

// A line's layers into the images a GPU backend puts together: per layer colour and priority, the sprite
// layer's window, the back screen's colour.
//
// An interlaced picture has twice the lines, and the hardware draws every other one each frame. Here the
// lines between are drawn too (as they would be drawn the frame after, if nothing moved), so that a moving
// background is whole in every frame instead of two half pictures a frame apart.
void Renderer::GpuLine(uint32 y) {
    const Vdp2Regs &regs2 = m_regs2;
    const size_t pixels = static_cast<size_t>(LineWidth()) * m_height;
    if (m_spriteWindowImage.size() != pixels) {
        for (auto &image : m_layerImage) {
            image.assign(pixels, 0);
        }
        m_spriteWindowImage.assign(pixels, 0);
    }
    m_backImage.resize(512);
    m_lines++;
    if (regs2.TVMD.IsInterlaced() && !m_exclusive) {
        const uint32 row = (y << 1) | regs2.TVSTAT.ODD;
        if (m_bothFields) {
            GpuRow(row ^ 1, y, true);
        }
        GpuRow(row, y, false);
    } else {
        GpuRow(y, y, false);
    }
}

void Renderer::GpuRow(uint32 row, uint32 y, bool otherField) {
    const Vdp2Regs &regs2 = m_regs2;
    const Vdp2Regs &state2 = m_regs2;
    const uint32 width = LineWidth();
    if (row >= m_height || row >= 512) {
        return;
    }
    uint32 *out[5];
    for (uint32 i = 0; i < 5; i++) {
        out[i] = &m_layerImage[i][static_cast<size_t>(row) * width];
    }
    if (!regs2.TVMD.DISP) {
        // (display off: the border colour, or black)
        m_backImage[row] = regs2.borderColorModeLatch ? (m_backColor & 0xFFFFFF) : 0;
        return;
    }
    m_backImage[row] = m_backColor | 0xFF000000;

    SpriteWindowLine(row);
    std::copy_n(m_spriteWindow.begin(), width, m_spriteWindowImage.begin() + static_cast<size_t>(row) * width);
    WindowLines(row);
    const bool rbg1 = regs2.bgEnabled[5];
    for (uint32 layer = 1; layer <= 5; layer++) {
        uint32 *line = out[layer - 1];
        if (layer == 1 && !otherField) {
            m_floorLines.resize(512 * 8);
            float *floor = &m_floorLines[(y & 511) * 8];
            std::fill_n(floor, 8, 0.0f);
            if (state2.layerEnabled[1]) {
                floor[4] = 2;
                if (m_gpuFloor && FloorLine(floor)) {
                    // (the backend draws this line of the rotation screen from the map itself)
                    m_layerLines[1]++;
                }
            }
        }
        // (debugging aid, SATRECOMP_HIDE_LAYERS=mask: bit 2 NBG0 .. bit 5 NBG3 not shown)
        static const uint32 hidden = getenv("SATRECOMP_HIDE_LAYERS") ? static_cast<uint32>(strtoul(getenv("SATRECOMP_HIDE_LAYERS"), nullptr, 0)) : 0;
        if (!state2.layerEnabled[layer] || (layer == 2 && rbg1) || (layer == 1 && m_floorLines[(y & 511) * 8 + 4] != 2) || ((hidden >> layer) & 1)) {
            std::fill_n(line, width, 0);
            continue;
        }
        if (layer == 1) {
            // (the rotation screen has one set of parameters per line of a field: the same line twice)
            RotationLine();
        } else if (otherField) {
            // The other field's line is half a line (of a field) above or below in the map
            const sint32 step = static_cast<sint32>(regs2.bgParams[layer - 1].scrollIncV);
            m_fetch[layer - 2].lastChar = 0xFFFFFFFF;
            NormalLine(layer - 2, (row & 1) ? step : -step);
            m_fetch[layer - 2].lastChar = 0xFFFFFFFF;
        } else {
            m_layerLines[layer]++;
            NormalLine(layer - 2);
        }
        const LayerLine &mine = m_layer[layer];
        for (uint32 x = 0; x < width; x++) {
            line[x] = (mine.color[x] & 0xFFFFFF) | (static_cast<uint32>(mine.priority[x]) << 24);
        }
        if (layer >= 2 && WideDots()) {
            SplitLine(layer - 2, row, line);
        }
    }
}

// Widescreen: a line of a scroll screen whose left and right parts belong at the picture's edges
void Renderer::SplitLine(uint32 layer, uint32 row, uint32 *line) const {
    const uint32 extra = WideDots(), width = LineWidth();
    for (const WideSplit &split : m_wideSplits) {
        if (split.layer != layer || row < split.rowFrom || row >= split.rowTo) {
            continue;
        }
        // (as drawn, the hardware picture's column x is at x + extra)
        for (uint32 x = 0; x < split.leftEnd; x++) {
            line[x] = line[x + extra];
        }
        std::fill(line + split.leftEnd, line + split.leftEnd + extra, 0u);
        for (uint32 x = width; x-- > split.rightStart + 2 * extra;) {
            line[x] = line[x - extra];
        }
        std::fill(line + split.rightStart + extra, line + split.rightStart + 2 * extra, 0u);
        return;
    }
}

void Renderer::GpuEndFrame() {
    const Vdp1Regs &regs1 = m_regs1;
    const Vdp2Regs &regs2 = m_regs2;
    const SpriteParams &params = regs2.spriteParams;
    Frame &frame = m_frame;
    if (m_spriteWindowImage.size() == static_cast<size_t>(LineWidth()) * m_height && m_onGpuFrame) {
        // The VDP1's framebuffer is narrower than a high resolution picture, and has a field's lines
        const bool exclusive = (regs2.TVMD.HRESOn & 0b100) != 0;
        const bool doubleWidth = !regs1.hdtvEnable && !regs1.pixel8Bits && (regs2.TVMD.HRESOn & 0b110) == 0b010;
        const bool interlaced = regs2.TVMD.IsInterlaced() && !m_exclusive;
        frame.spriteWidth = (m_width >> (doubleWidth || exclusive ? 1 : 0)) + 2 * m_wideExtra;
        frame.spriteHeight = m_height >> (interlaced ? 1 : 0);
        frame.width = LineWidth();
        // (only a scene with the 3D floor is shown beyond the hardware's picture)
        frame.blankSides = m_regs2.layerEnabled[1] ? 0 : WideDots();
        frame.height = m_height;
        frame.interlaced = interlaced;
        for (uint32 i = 0; i < 5; i++) {
            frame.layers[i] = m_layerImage[i].data();
        }
        frame.spriteWindow = m_spriteWindowImage.data();
        frame.back = m_backImage.data();
        m_colorImage.resize(2048);
        Colors(m_colorImage.data());
        frame.colors = m_colorImage.data();
        frame.spriteType = params.type;
        frame.spriteMixed = params.mixedFormat;
        frame.spriteWindowUsed = params.useSpriteWindow;
        frame.spriteWindowEnabled = params.spriteWindowEnabled;
        frame.spriteWindowInverted = params.spriteWindowInverted;
        frame.spritesEnabled = m_regs2.layerEnabled[0];
        frame.spriteColorOffset = params.colorDataOffset;
        for (uint32 i = 0; i < 8; i++) {
            frame.spritePriorities[i] = params.priorities[i];
        }
        frame.shadow[0] = false;
        for (uint32 i = 0; i < 5; i++) {
            frame.shadow[i + 1] = regs2.bgParams[i].shadowEnable;
        }
        frame.shadow[6] = regs2.backScreenParams.shadowEnable;
        m_floorLines.resize(512 * 8);
        m_vram2Image.assign(m_mem.vram2, m_mem.vram2 + 0x80000);
        frame.floor.lines = m_floorLines.data();
        frame.floor.lineCount = frame.spriteHeight;
        frame.floor.dots = (m_width > 352 ? m_width >> 1 : m_width) + 2 * m_wideExtra;
        frame.floor.vram = m_vram2Image.data();
        FloorSettings(frame);
        for (uint32 i = 0; i < 7; i++) {
            const auto &offset = regs2.colorOffset[regs2.colorOffsetSelect[i]];
            const bool on = regs2.colorOffsetEnable[i] && offset.nonZero;
            frame.colorOffset[i][0] = on ? static_cast<int16_t>(bit::sign_extend<9>(offset.r)) : 0;
            frame.colorOffset[i][1] = on ? static_cast<int16_t>(bit::sign_extend<9>(offset.g)) : 0;
            frame.colorOffset[i][2] = on ? static_cast<int16_t>(bit::sign_extend<9>(offset.b)) : 0;
        }
        m_onGpuFrame(frame);
    }
    frame.events.clear();
    frame.quads.clear();
    frame.vram.clear();
    frame.resetSprites = false;
}

// A GPU backend's VDP1 framebuffer (the one on display, at the original resolution) against the reference's
void Renderer::CompareSprites(const uint16_t *words, uint32_t width, uint32_t height) {
    const uint8 *fb = m_mem.fb[m_displayFB];
    const uint32 pitch = m_regs1.fbSizeH;
    static const char *dump = getenv("SATRECOMP_GPU_DUMP"); // FRAME:PREFIX: both framebuffers of that frame, as raw 16-bit
    const bool dumping = dump && static_cast<uint64_t>(atoll(dump)) == m_frames && strchr(dump, ':');
    std::vector<uint16_t> reference;
    for (uint32 y = 0; y < height; y++) {
        for (uint32 x = 0; x < width; x++) {
            const uint32 at = ((y * pitch + x) * 2) & 0x3FFFE;
            const uint16_t theirs = static_cast<uint16_t>((fb[at] << 8) | fb[at + 1]);
            const uint16_t mine = words[y * width + x];
            if (dumping) {
                reference.push_back(theirs);
            }
            if (!mine && !theirs) {
                continue;
            }
            m_gpuPixels++;
            if (mine == theirs) {
                m_gpuSame++;
            } else if (!theirs) {
                m_gpuOnlyHere++;
            } else if (!mine) {
                m_gpuOnlyReference++;
            } else {
                // (close: RGB colours no more than 2 of 31 apart in any channel)
                bool close = (mine & 0x8000) && (theirs & 0x8000);
                for (int shift = 0; shift < 15 && close; shift += 5) {
                    close = std::abs(((mine >> shift) & 31) - ((theirs >> shift) & 31)) <= 2;
                }
                (close ? m_gpuClose : m_gpuOther)++;
            }
        }
    }
    if (dumping) {
        const std::string prefix = strchr(dump, ':') + 1;
        for (int i = 0; i < 2; i++) {
            if (FILE *f = fopen((prefix + (i ? "reference.raw" : "gpu.raw")).c_str(), "wb")) {
                fwrite(i ? reference.data() : words, 2, static_cast<size_t>(width) * height, f);
                fclose(f);
            }
        }
        fprintf(stderr, "video: frame %llu's VDP1 framebuffers written, %ux%u\n", static_cast<unsigned long long>(m_frames), width, height);
    }
}

} // namespace satrecomp::video
