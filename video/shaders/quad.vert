#version 450
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// A VDP1 quad: drawn as the rectangle around it; everything the fragment shader needs is the same at all
// its corners.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_ab;
layout(location = 2) in vec4 a_cd;
layout(location = 3) in vec4 a_clip;
layout(location = 4) in vec4 a_clipOut;
layout(location = 5) in uvec4 a_tex;     // address, width | height << 10 | lines << 18, mode | flags << 16, colour
layout(location = 6) in uvec4 a_gouraud;

layout(set = 1, binding = 0, std140) uniform VertexUniforms {
    vec4 size; // xy: the VDP1 picture's size in its pixels
};

layout(location = 0) flat out vec4 v_ab;
layout(location = 1) flat out vec4 v_cd;
layout(location = 2) flat out vec4 v_clip;
layout(location = 3) flat out vec4 v_clipOut;
layout(location = 4) flat out uvec4 v_tex;
layout(location = 5) flat out uvec4 v_gouraud;

void main() {
    v_ab = a_ab;
    v_cd = a_cd;
    v_clip = a_clip;
    v_clipOut = a_clipOut;
    v_tex = a_tex;
    v_gouraud = a_gouraud;
    gl_Position = vec4(a_position.x / size.x * 2.0 - 1.0, 1.0 - a_position.y / size.y * 2.0, 0.0, 1.0);
}
