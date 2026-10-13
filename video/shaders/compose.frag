#version 450
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The picture: what the VDP2 makes of the sprite framebuffer and its own layers. For every pixel the layer
// with the highest priority that has something there shows (between equals: sprites, RBG0, NBG0..NBG3); where
// none has, the back screen. Sprite shadows darken what is below them.
layout(location = 0) in vec2 uv;

layout(set = 2, binding = 0) uniform usampler2D sprites; // the VDP1 framebuffer on display
layout(set = 2, binding = 1) uniform sampler2D colors;   // colour RAM, 2048 colours in a row
layout(set = 2, binding = 2) uniform sampler2D rbg0;     // layers: colour, priority in alpha (0: nothing)
layout(set = 2, binding = 3) uniform sampler2D nbg0;
layout(set = 2, binding = 4) uniform sampler2D nbg1;
layout(set = 2, binding = 5) uniform sampler2D nbg2;
layout(set = 2, binding = 6) uniform sampler2D nbg3;
layout(set = 2, binding = 7) uniform sampler2D window;   // where the sprite layer is cut out
layout(set = 2, binding = 8) uniform sampler2D back;     // per row: back screen colour; alpha 0: display off
layout(set = 2, binding = 9) uniform usampler2D vram2;   // VDP2 video memory, 1024 bytes a row
layout(set = 2, binding = 10) uniform sampler2D floorLines; // the rotation screen, per line: [0] where its first
                                                            // and last dots look in the map, [1].x what the line is

layout(set = 3, binding = 0, std140) uniform FragmentUniforms {
    vec4 size;      // xy: the VDP2 picture's size; zw: the sprite target's size
    ivec4 sprite;   // sprite type, mixed RGB/palette, flags (1 window used, 2 enabled, 4 inverted, 8 sprites on), colour offset
    ivec4 priLow;   // the sprite priority registers
    ivec4 priHigh;
    ivec4 misc;     // x: which layers take sprite shadows (bit 1 RBG0 .. bit 5 NBG3, bit 6 back); y: samples a side;
                    // z: columns at each side of the picture that show black (widescreen, in a scene without)
    vec4 colorOffset[7]; // added to the colour of the layer shown (sprites, RBG0, NBG0..3, back), for fades
    ivec4 floorSize;     // the rotation screen: lines of a field, dots a line, flags, priority
    ivec4 floorMore;     // its colour offset, the extra bits of its character numbers and palettes, memory access
    uvec4 floorPlanes[4]; // the addresses of the 4x4 planes of its map
};

layout(location = 0) out vec4 out_color;

// How each of the 16 sprite data formats packs colour and priority: colour bits, priority shift, priority
// bits, and whether bit 15 is the shadow / window bit
const ivec4 kFormats[16] = ivec4[16](
    ivec4(11, 14, 2, 0), ivec4(11, 13, 3, 0), ivec4(11, 14, 1, 1), ivec4(11, 13, 2, 1),
    ivec4(10, 13, 2, 1), ivec4(11, 12, 3, 1), ivec4(10, 12, 3, 1), ivec4(9, 12, 3, 1),
    ivec4(7, 7, 1, 0),   ivec4(6, 7, 1, 0),   ivec4(6, 6, 2, 0),   ivec4(6, 0, 0, 0),
    ivec4(8, 7, 1, 0),   ivec4(8, 7, 1, 0),   ivec4(8, 6, 2, 0),   ivec4(8, 0, 0, 0));

int spritePriority(int index) {
    return index < 4 ? priLow[index] : priHigh[index - 4];
}

uint vram2Byte(uint address) {
    return texelFetch(vram2, ivec2(int(address & 1023u), int((address >> 10) & 511u)), 0).r;
}

// The rotation screen at `at`, read from its map in video memory: a plane seen in perspective, each line of
// the screen a straight line across the map. Lines are given for the original's resolution; between two of
// them the positions are taken in between, so the plane is drawn as finely as the picture is. Returns the
// colour, and the priority in alpha (0: nothing); negative alpha: this line is in the layer's image instead.
vec4 floorLayer(vec2 at) {
    int lines = floorSize.x;
    float line = at.y * float(lines) - 0.5;
    int l0 = clamp(int(floor(line)), 0, lines - 1), l1 = min(l0 + 1, lines - 1);
    float kind0 = texelFetch(floorLines, ivec2(1, l0), 0).x, kind1 = texelFetch(floorLines, ivec2(1, l1), 0).x;
    int nearest = clamp(int(floor(at.y * float(lines))), 0, lines - 1);
    float kind = texelFetch(floorLines, ivec2(1, nearest), 0).x;
    if (kind > 1.5) {
        return vec4(0.0, 0.0, 0.0, -1.0);
    }
    if (kind < 0.5) {
        return vec4(0.0);
    }
    vec4 ends;
    if (kind0 > 0.5 && kind0 < 1.5 && kind1 > 0.5 && kind1 < 1.5) {
        ends = mix(texelFetch(floorLines, ivec2(0, l0), 0), texelFetch(floorLines, ivec2(0, l1), 0), clamp(line - float(l0), 0.0, 1.0));
    } else {
        ends = texelFetch(floorLines, ivec2(0, nearest), 0);
    }
    float dots = float(floorSize.y);
    vec2 where = mix(ends.xy, ends.zw, (at.x * dots - 0.5) / max(dots - 1.0, 1.0));
    ivec2 m = ivec2(floor(where));

    int flags = floorSize.z;
    uint fourCell = uint(flags & 1);
    bool twoWord = (flags & 2) != 0, extended = (flags & 4) != 0, wide = (flags & 8) != 0;
    int over = (flags >> 8) & 3;
    int pageShiftH = (flags >> 12) & 1, pageShiftV = (flags >> 13) & 1;
    if (over != 0) {
        // (outside the map there is nothing)
        ivec2 size = over == 2 ? ivec2(512) : ivec2(2048 << pageShiftH, 2048 << pageShiftV);
        if (m.x < 0 || m.y < 0 || m.x >= size.x || m.y >= size.y) {
            return vec4(0.0);
        }
    }
    uint x = uint(m.x), y = uint(m.y);
    uint plane = ((x >> (9 + pageShiftH)) & 3u) + (((y >> (9 + pageShiftV)) & 3u) << 2);
    uint planeAddress = floorPlanes[plane >> 2][plane & 3u];
    uint page = (((x >> 9) & 1u) & uint(pageShiftH)) + ((((y >> 9) & 1u) & uint(pageShiftV)) << 1);
    uint charIndex = (((x >> 3) & 63u) >> fourCell) + ((((y >> 3) & 63u) >> fourCell) << (6u - fourCell));
    uint pageShift = (fourCell != 0u ? 11u : 13u) + (twoWord ? 1u : 0u);
    uint pageAddress = planeAddress + (page << pageShift);
    uint access = uint(floorMore.w);

    // The pattern name: which character, its palette and flips
    uint number = 0u, palette = 0u;
    bool flipH = false, flipV = false, special = false;
    if (twoWord) {
        uint address = pageAddress + charIndex * 4u;
        if ((access & (1u << ((address >> 17) & 3u))) != 0u) {
            uint hi = (vram2Byte(address) << 8) | vram2Byte(address + 1u), lo = (vram2Byte(address + 2u) << 8) | vram2Byte(address + 3u);
            number = lo & 0x7FFFu;
            palette = hi & 0x7Fu;
            special = (hi & 0x2000u) != 0u;
            flipH = (hi & 0x4000u) != 0u;
            flipV = (hi & 0x8000u) != 0u;
        }
    } else {
        uint address = pageAddress + charIndex * 2u;
        if ((access & (1u << ((address >> 17) & 3u))) != 0u) {
            uint word = (vram2Byte(address) << 8) | vram2Byte(address + 1u);
            uint more = uint(floorMore.y);
            uint numberBits = extended ? 12u : 10u;
            uint moreStart = 2u * fourCell + (extended ? 2u : 0u);
            number = (word & ((1u << numberBits) - 1u)) << (2u * fourCell);
            number |= ((more & 0x1Fu) >> moreStart) << (10u + moreStart);
            if (fourCell != 0u) {
                number |= more & 3u;
            }
            number &= 0x7FFFu;
            palette = wide ? ((word >> 12) & 7u) << 4 : (((word >> 12) & 0xFu) | uint(floorMore.z));
            palette &= 0x7Fu;
            special = (flags & 64) != 0;
            flipH = !extended && (word & 0x400u) != 0u;
            flipV = !extended && (word & 0x800u) != 0u;
        }
    }
    uint cell = ((x >> 3) & fourCell) + (((y >> 3) & fourCell) << 1);
    uint dotX = x & 7u, dotY = y & 7u;
    if (flipH) {
        dotX ^= 7u;
        cell ^= fourCell;
    }
    if (flipV) {
        dotY ^= 7u;
        cell ^= fourCell << 1;
    }
    uint row = ((number + (wide ? cell << 1 : cell)) << 5) + dotY * (wide ? 8u : 4u);
    uint dot = 0u;
    if ((access & (16u << ((row >> 17) & 3u))) != 0u) {
        dot = wide ? vram2Byte(row + dotX) : (vram2Byte(row + (dotX >> 1)) >> ((~dotX & 1u) * 4u)) & 0xFu;
    }
    if (dot == 0u && (flags & 16) != 0) {
        return vec4(0.0);
    }
    uint index = uint(floorMore.x) + (wide ? (((palette << 4) & 0x700u) | dot) : ((palette << 4) | dot));
    int priority = floorSize.w;
    if ((flags & 32) != 0) {
        priority = (priority & ~1) | (special ? 1 : 0);
    }
    return vec4(texelFetch(colors, ivec2(int(index & 2047u), 0), 0).rgb, float(priority) / 255.0);
}

vec3 pixel(vec2 at) {
    ivec2 cell = clamp(ivec2(floor(at * size.xy)), ivec2(0), ivec2(size.xy) - 1);
    if (cell.x < misc.z || cell.x >= int(size.x) - misc.z) {
        return vec3(0.0);
    }
    vec4 backColor = texelFetch(back, ivec2(0, cell.y), 0);
    if (backColor.a < 0.5) {
        return backColor.rgb;
    }

    // The sprite layer's pixel
    uint word = texelFetch(sprites, clamp(ivec2(floor(at * size.zw)), ivec2(0), ivec2(size.zw) - 1), 0).r;
    int type = sprite.x;
    bool mixed = sprite.y != 0;
    bool windowUsed = (sprite.z & 1) != 0;
    vec3 spriteColor = vec3(0.0);
    int priority = 0;
    int special = 0; // 0: nothing, 1: a pixel, 2: a shadow
    bool shadowBit = false;
    bool cut = texelFetch(window, cell, 0).r > 0.5;
    bool rgb = mixed && (word & 0x8000u) != 0u;
    bool rgbNothing = rgb && (type >= 8 ? (word & 0xFFu) == 0u : (type >= 2 && windowUsed && (word & 0x7FFFu) == 0u));
    if (cut || rgbNothing) {
    } else if (rgb) {
        spriteColor = vec3(word & 31u, (word >> 5) & 31u, (word >> 10) & 31u) / 31.0;
        // (5 bits to 8 as the hardware does: the 5 bits shifted up, not stretched)
        spriteColor = floor(spriteColor * 31.0 + 0.5) * 8.0 / 255.0;
        priority = priLow.x;
        special = 1;
    } else {
        ivec4 format = kFormats[type & 15];
        uint colorData = word & ((1u << uint(format.x)) - 1u);
        int priorityIndex = int((word >> uint(format.y)) & ((1u << uint(format.z)) - 1u));
        shadowBit = format.w != 0 && (word & 0x8000u) != 0u;
        special = (word & 0x7FFFu) == 0u ? 0 : (colorData == (1u << uint(format.x)) - 2u ? 2 : 1);
        if (windowUsed && (sprite.z & 2) != 0 && shadowBit != ((sprite.z & 4) != 0)) {
            shadowBit = true;
            special = 0;
        } else {
            spriteColor = texelFetch(colors, ivec2(int((uint(sprite.w) + colorData) & 2047u), 0), 0).rgb;
            priority = (special == 0 && !shadowBit) ? 0 : spritePriority(priorityIndex);
        }
    }

    // The layer on top
    int top = 6;
    int topPriority = 0;
    vec3 color = backColor.rgb;
    if ((sprite.z & 8) != 0 && special == 1) {
        if (priority > 0) {
            top = 0;
            color = spriteColor;
        }
        topPriority = priority;
    }
    vec4 layer;
    int layerPriority;
    layer = floorLayer(at);
    if (layer.a < 0.0) {
        layer = texelFetch(rbg0, cell, 0);
    }
    layerPriority = int(layer.a * 255.0 + 0.5);
    if (layerPriority > topPriority) { top = 1; topPriority = layerPriority; color = layer.rgb; }
    layer = texelFetch(nbg0, cell, 0);
    layerPriority = int(layer.a * 255.0 + 0.5);
    if (layerPriority > topPriority) { top = 2; topPriority = layerPriority; color = layer.rgb; }
    layer = texelFetch(nbg1, cell, 0);
    layerPriority = int(layer.a * 255.0 + 0.5);
    if (layerPriority > topPriority) { top = 3; topPriority = layerPriority; color = layer.rgb; }
    layer = texelFetch(nbg2, cell, 0);
    layerPriority = int(layer.a * 255.0 + 0.5);
    if (layerPriority > topPriority) { top = 4; topPriority = layerPriority; color = layer.rgb; }
    layer = texelFetch(nbg3, cell, 0);
    layerPriority = int(layer.a * 255.0 + 0.5);
    if (layerPriority > topPriority) { top = 5; topPriority = layerPriority; color = layer.rgb; }

    // A shadow pixel of the sprite layer halves the brightness of the layer shown, if that layer takes shadows
    if (special == 2 || (!windowUsed && shadowBit)) {
        bool takes = top == 0 ? shadowBit : ((misc.x >> top) & 1) != 0;
        if (takes) {
            color = floor(color * 255.0 + 0.5) * 0.5 / 255.0;
            color = floor(color * 255.0) / 255.0;
        }
    }
    return clamp(color + colorOffset[top].rgb, 0.0, 1.0);
}

void main() {
    int n = max(misc.y, 1);
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    vec3 sum = vec3(0.0);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            vec2 offset = (vec2(float(i), float(j)) + 0.5) / float(n) - 0.5;
            sum += pixel(uv + dx * offset.x + dy * offset.y);
        }
    }
    out_color = vec4(sum / float(n * n), 1.0);
}
