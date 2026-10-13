#version 450
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// A VDP1 quad's pixels. The target holds what the VDP1's framebuffer holds: a 16-bit value per pixel, an RGB
// colour (bit 15 set) or palette data with priority bits, 0 for nothing.
layout(location = 0) flat in vec4 v_ab;
layout(location = 1) flat in vec4 v_cd;
layout(location = 2) flat in vec4 v_clip;
layout(location = 3) flat in vec4 v_clipOut;
layout(location = 4) flat in uvec4 v_tex;
layout(location = 5) flat in uvec4 v_gouraud;

layout(set = 2, binding = 0) uniform usampler2D vram; // VDP1 video memory, 1024 bytes a row

layout(set = 3, binding = 0, std140) uniform FragmentUniforms {
    vec4 scale; // x: target pixels per VDP1 pixel
};

layout(location = 0) out uint out_word;

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

uint fetch8(uint address) {
    return texelFetch(vram, ivec2(int(address & 1023u), int((address >> 10) & 511u)), 0).r;
}
uint fetch16(uint address) {
    return (fetch8(address) << 8) | fetch8(address + 1u);
}

// Where p lies in the quad a, b, c, d: p = a + u (b - a) + v (d - a) + u v (a - b + c - d). The hardware
// steps v down the edges a-d and b-c and u along the line between.
bool locate(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d, out vec2 uv) {
    vec2 e = b - a, f = d - a, g = a - b + c - d, h = p - a;
    float k2 = cross2(g, f);
    float k1 = cross2(e, f) + cross2(h, g);
    float k0 = cross2(h, e);
    float w = k1 * k1 - 4.0 * k0 * k2;
    if (w < 0.0) {
        return false;
    }
    w = sqrt(w);
    float q = -0.5 * (k1 + (k1 < 0.0 ? -w : w));
    float roots[2] = float[2](k0 / q, q / k2);
    for (int i = 0; i < 2; i++) {
        float v = roots[i];
        if (!(v >= 0.0 && v <= 1.0)) {
            continue;
        }
        vec2 den = e + g * v, num = h - f * v;
        float u;
        if (abs(den.x) > abs(den.y)) {
            u = num.x / den.x;
        } else if (abs(den.y) > 1e-6) {
            u = num.y / den.y;
        } else {
            u = 0.5; // (a side of no length: any column)
            if (dot(num, num) > 0.5) {
                continue;
            }
        }
        if (u >= 0.0 && u <= 1.0) {
            uv = vec2(u, v);
            return true;
        }
    }
    return false;
}

// Which of `count` texels shows at `along` (0..1) of `length` pixels. Shrinking, each pixel takes the texel
// under its centre; stretching, the first pixel shows the first texel and the last the last.
uint step_texel(float along, float length, uint count) {
    float n = float(count);
    float texel;
    if (length <= n - 1.0) {
        texel = floor(along * n);
    } else if (length <= 1.0) {
        texel = 0.0;
    } else {
        float pixel = along * length - 0.5;
        texel = floor((pixel * (n - 1.0) - 0.5) / (length - 1.0) + 0.5);
    }
    return uint(clamp(texel, 0.0, n - 1.0));
}

void main() {
    vec2 p = gl_FragCoord.xy / scale.x;
    if (p.x < v_clip.x || p.y < v_clip.y || p.x >= v_clip.z || p.y >= v_clip.w) {
        discard;
    }
    if (p.x >= v_clipOut.x && p.y >= v_clipOut.y && p.x < v_clipOut.z && p.y < v_clipOut.w) {
        discard;
    }
    uint mode = v_tex.z & 0xFFFFu;
    uint flags = v_tex.z >> 16;
    if ((flags & 8u) != 0u) { // erasing: the value as it is
        out_word = v_tex.w;
        return;
    }
    vec2 uv;
    if (!locate(p, v_ab.xy, v_ab.zw, v_cd.xy, v_cd.zw, uv)) {
        discard;
    }
    ivec2 pixel = ivec2(floor(p));
    if ((mode & 0x100u) != 0u && ((pixel.x ^ pixel.y) & 1) != 0) { // mesh: every other pixel
        discard;
    }

    uint color;
    if ((flags & 1u) != 0u) {
        color = v_tex.w;
    } else {
        uint width = v_tex.y & 0x3FFu, height = (v_tex.y >> 10) & 0xFFu;
        uint tx = 0u, ty = 0u;
        if (width != 0u) {
            // The hardware draws the quad as `lines` lines from its edge a-d to its edge b-c, each `across`
            // pixels long, and steps through the character's rows and columns as it goes
            vec2 span = abs(mix(v_ab.zw, v_cd.xy, uv.y) - mix(v_ab.xy, v_cd.zw, uv.y));
            float across = max(max(span.x, span.y), 1.0);
            float lines = float(max(v_tex.y >> 18, 1u));
            bool halved = (mode & 0x1000u) != 0u && across < float(width); // high speed shrink: every other column
            uint columns = halved ? width / 2u : width;
            tx = step_texel(uv.x, across, columns);
            if ((flags & 2u) != 0u) {
                tx = columns - 1u - tx;
            }
            if (halved) {
                tx = (tx << 1) | ((flags >> 4) & 1u);
            }
            ty = step_texel(uv.y, lines, max(height, 1u));
            if ((flags & 4u) != 0u) {
                ty = max(height, 1u) - 1u - ty;
            }
        }
        uint index = tx + ty * max(width, 1u);
        uint address = v_tex.x;
        bool transparent, endCode;
        switch ((mode >> 3) & 7u) {
        case 0u: // 16 colours, of the colour bank
            color = (fetch8(address + (index >> 1)) >> ((~tx & 1u) * 4u)) & 0xFu;
            endCode = color == 0xFu;
            transparent = color == 0u;
            color |= v_tex.w & 0xFFF0u;
            break;
        case 1u: // 16 colours, through a table in video memory
            color = (fetch8(address + (index >> 1)) >> ((~tx & 1u) * 4u)) & 0xFu;
            endCode = color == 0xFu;
            transparent = color == 0u;
            color = fetch16(color * 2u + (v_tex.w << 3));
            break;
        case 2u:
            color = fetch8(address + index);
            endCode = color == 0xFFu;
            transparent = color == 0u;
            color = (color & 0x3Fu) | (v_tex.w & 0xFFC0u);
            break;
        case 3u:
            color = fetch8(address + index);
            endCode = color == 0xFFu;
            transparent = color == 0u;
            color = (color & 0x7Fu) | (v_tex.w & 0xFF80u);
            break;
        case 4u:
            color = fetch8(address + index);
            endCode = color == 0xFFu;
            transparent = color == 0u;
            color |= v_tex.w & 0xFF00u;
            break;
        default: // RGB
            color = fetch16(address + index * 2u);
            endCode = color == 0x7FFFu;
            transparent = (color & 0x8000u) == 0u;
            break;
        }
        if ((endCode && (mode & 0x80u) == 0u) || (transparent && (mode & 0x40u) == 0u)) {
            discard;
        }
    }

    if ((mode & 4u) != 0u) {
        // Gouraud shading: the four corners' colours, spread over the quad, shift each channel up or down
        vec3 ga = vec3(v_gouraud.x & 31u, (v_gouraud.x >> 5) & 31u, (v_gouraud.x >> 10) & 31u);
        vec3 gb = vec3(v_gouraud.y & 31u, (v_gouraud.y >> 5) & 31u, (v_gouraud.y >> 10) & 31u);
        vec3 gc = vec3(v_gouraud.z & 31u, (v_gouraud.z >> 5) & 31u, (v_gouraud.z >> 10) & 31u);
        vec3 gd = vec3(v_gouraud.w & 31u, (v_gouraud.w >> 5) & 31u, (v_gouraud.w >> 10) & 31u);
        // (the corners' colours are those of the first and last pixel of the first and last line)
        vec2 span = abs(mix(v_ab.zw, v_cd.xy, uv.y) - mix(v_ab.xy, v_cd.zw, uv.y));
        vec2 count = vec2(max(max(span.x, span.y), 1.0), float(max(v_tex.y >> 18, 1u)));
        vec2 at = clamp((uv * count - 0.5) / max(count - 1.0, vec2(1.0)), 0.0, 1.0);
        vec3 shade = mix(mix(ga, gb, at.x), mix(gd, gc, at.x), at.y);
        ivec3 rgb = ivec3(color & 31u, (color >> 5) & 31u, (color >> 10) & 31u);
        rgb = clamp(rgb + ivec3(floor(shade + 0.5)) - 16, 0, 31);
        color = (color & 0x8000u) | uint(rgb.x) | (uint(rgb.y) << 5) | (uint(rgb.z) << 10);
    }
    if ((mode & 3u) == 2u) { // half luminance
        color = (color & 0x8000u) | ((color >> 1) & 0x3DEFu);
    }
    out_word = color;
}
