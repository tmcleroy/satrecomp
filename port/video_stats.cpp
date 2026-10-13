// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Debugging aid (--video-stats): what the game asks of the video hardware, summed over the run. For planning
// and checking the native renderer: which VDP1 commands and drawing modes, which VDP2 screens and settings.
#include "port.h"

#include <map>
#include <set>
#include <string>

namespace port {

namespace {
    struct Stats {
        uint64_t frames = 0;
        std::map<std::string, uint64_t> vdp1;      // commands by kind and drawing mode
        std::map<std::string, uint64_t> vdp2;      // frames by setting
        uint64_t vdp1Commands = 0, vdp1Max = 0;
    } g;

    const char *kCommands[16] = {"normal sprite", "scaled sprite", "distorted sprite", "distorted sprite",
                                 "polygon",       "polyline",      "line",             "polyline",
                                 "user clip",     "system clip",   "local coords",     "user clip",
                                 "?",             "?",             "?",                "?"};
    const char *kColorModes[8] = {"16 colours (bank)", "16 colours (table)", "64 colours", "128 colours",
                                  "256 colours",        "RGB",                "?",          "?"};
} // namespace

void VideoStatsFrame(Port &p) {
    auto &bus = p.saturn->mainBus;
    auto vram1 = [&](uint32_t a) { return bus.Peek<uint16_t>(0x05C00000 + (a & 0x7FFFE)); };
    auto reg2 = [&](uint32_t a) { return bus.Peek<uint16_t>(0x05F80000 + a); };
    g.frames++;

    // VDP1: walk the command table as the hardware does
    uint32_t at = 0;
    uint32_t returnTo = 0xFFFFFFFF;
    uint64_t count = 0;
    for (unsigned guard = 0; guard < 4096; guard++) {
        const uint16_t ctrl = vram1(at);
        if (ctrl & 0x8000) {
            break; // end of the table
        }
        const unsigned jump = (ctrl >> 12) & 7;
        if (!(jump & 4)) { // not skipped
            const unsigned cmd = ctrl & 0xF;
            const uint16_t pmod = vram1(at + 4);
            std::string key = kCommands[cmd];
            if (cmd < 8) {
                if (cmd < 4) {
                    key += std::string(", ") + kColorModes[(pmod >> 3) & 7];
                }
                static const char *kCalc[8] = {"replace", "shadow", "half luminance", "half transparent",
                                               "gouraud", "?",      "gouraud + half luminance", "gouraud + half transparent"};
                key += std::string(", ") + kCalc[pmod & 7];
                if (pmod & 0x100) key += ", mesh";
                if (pmod & 0x8000) key += ", msb on";
                if (pmod & 0x400) key += ", user clip";
                if (cmd < 4 && !(pmod & 0x40)) key += ", transparent pixels";
                if (cmd < 4 && !(pmod & 0x80)) key += ", end codes";
                if (pmod & 0x1000) key += ", high speed shrink";
            }
            g.vdp1[key]++;
            count++;
        }
        switch (jump & 3) {
        case 0: at += 0x20; break;
        case 1: at = static_cast<uint32_t>(vram1(at + 2)) << 3; break;
        case 2:
            returnTo = at + 0x20;
            at = static_cast<uint32_t>(vram1(at + 2)) << 3;
            break;
        case 3:
            if (returnTo == 0xFFFFFFFF) {
                guard = 4096;
            } else {
                at = returnTo;
                returnTo = 0xFFFFFFFF;
            }
            break;
        }
        at &= 0x7FFFF;
    }
    g.vdp1Commands += count;
    g.vdp1Max = std::max(g.vdp1Max, count);

    // VDP2: the settings that decide what a renderer has to do
    auto note = [&](const std::string &s) { g.vdp2[s]++; };
    char text[128];
    const uint16_t tvmd = reg2(0x00), bgon = reg2(0x20), chctla = reg2(0x28), chctlb = reg2(0x2A);
    snprintf(text, sizeof(text), "TVMD %04X (display %s, interlace %u, %s lines, hres mode %u)", tvmd, tvmd & 0x8000 ? "on" : "off",
             (tvmd >> 6) & 3, ((tvmd >> 4) & 3) == 0 ? "224" : ((tvmd >> 4) & 3) == 1 ? "240" : "256", tvmd & 7);
    note(text);
    static const char *kScreens[6] = {"NBG0", "NBG1", "NBG2", "NBG3", "RBG0", "RBG1"};
    std::string screens = "screens on:";
    for (int i = 0; i < 6; i++) {
        if (bgon & (1 << i)) {
            screens += std::string(" ") + kScreens[i];
        }
    }
    note(screens);
    snprintf(text, sizeof(text), "CHCTLA %04X CHCTLB %04X (NBG0 %s %u colours bits, NBG1 %s, RBG0 %s)", chctla, chctlb,
             chctla & 2 ? "bitmap" : "cells", (chctla >> 4) & 7, chctla & 0x200 ? "bitmap" : "cells",
             chctlb & 0x200 ? "bitmap" : "cells");
    note(text);
    snprintf(text, sizeof(text), "RAMCTL %04X (colour RAM mode %u, coefficient table in %s)", reg2(0x0E), (reg2(0x0E) >> 12) & 3,
             reg2(0x0E) & 0x8000 ? "colour RAM" : "VRAM");
    note(text);
    snprintf(text, sizeof(text), "SPCTL %04X (sprite type %X, %s, colour calc cond %u)", reg2(0xE0), reg2(0xE0) & 0xF,
             reg2(0xE0) & 0x20 ? "mixed RGB/palette" : "palette only", (reg2(0xE0) >> 12) & 3);
    note(text);
    snprintf(text, sizeof(text), "CCCTL %04X (colour calculation enables)", reg2(0xEC));
    note(text);
    snprintf(text, sizeof(text), "SFSEL/SFCODE %04X %04X, MZCTL %04X, SCRCTL %04X (line/vertical cell scroll), LNCLEN %04X, SDCTL %04X",
             reg2(0x24), reg2(0x26), reg2(0x22), reg2(0x9A), reg2(0xE8), reg2(0xE2));
    note(text);
    snprintf(text, sizeof(text), "RPMD %04X KTCTL %04X (rotation parameter mode, coefficient tables)", reg2(0xB0), reg2(0xB4));
    note(text);
    snprintf(text, sizeof(text), "windows: WCTLA-D %04X %04X %04X %04X", reg2(0xD0), reg2(0xD2), reg2(0xD4), reg2(0xD6));
    note(text);
    snprintf(text, sizeof(text), "colour offset: CLOFEN %04X", reg2(0x110));
    note(text);
    snprintf(text, sizeof(text), "priorities: sprite %04X %04X %04X %04X, NBG %04X %04X, RBG0 %04X", reg2(0xF0), reg2(0xF2),
             reg2(0xF4), reg2(0xF6), reg2(0xF8), reg2(0xFA), reg2(0xFC));
    note(text);
}

void VideoStatsReport() {
    if (!g.frames) {
        return;
    }
    fprintf(stderr, "video: %llu frames; VDP1 commands per frame: average %.1f, most %llu\n",
            static_cast<unsigned long long>(g.frames), static_cast<double>(g.vdp1Commands) / g.frames,
            static_cast<unsigned long long>(g.vdp1Max));
    for (auto &[key, n] : g.vdp1) {
        fprintf(stderr, "video: VDP1 %9llu  %s\n", static_cast<unsigned long long>(n), key.c_str());
    }
    for (auto &[key, n] : g.vdp2) {
        if (n * 200 >= g.frames) { // (settings seen in at least 0.5% of frames)
            fprintf(stderr, "video: VDP2 %6llu frames  %s\n", static_cast<unsigned long long>(n), key.c_str());
        }
    }
}

} // namespace port
