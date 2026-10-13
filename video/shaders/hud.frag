#version 450
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The readout over the picture (port/hud.h): a picture with alpha, blended over the output.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 2, binding = 0) uniform sampler2D hud;

void main() {
    color = texture(hud, uv);
}
