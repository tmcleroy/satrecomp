#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""Copies the Display page's shader presets out of RetroArch's slang-shaders (https://github.com/libretro/slang-shaders),
with every file each one uses (its passes, what they #include, its textures, the presets it #references), into a
folder at the same places: the game's shaders/ (port/shading.h). The presets are kShaderPresets in port/main.cpp.

  shaders.py SLANG_SHADERS OUT
"""

import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def presets():
    """kShaderPresets, from port/main.cpp"""
    with open(os.path.join(ROOT, "port", "main.cpp")) as f:
        text = f.read()
    table = re.search(r"kShaderPresets\[\] = \{(.*?)\};", text, re.S).group(1)
    return re.findall(r'"([^"]+)"', table)


def uses(path):
    """The files a preset or a shader names, as paths beside it"""
    here = os.path.dirname(path)
    found = []
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    if path.endswith(".slangp"):
        values = {}
        for line in lines:
            reference = re.match(r'\s*#reference\s+"([^"]+)"', line)
            if reference:
                found.append(reference.group(1))
                continue
            line = line.split("#", 1)[0] if not line.lstrip().startswith("#") else ""
            match = re.match(r'\s*([A-Za-z0-9_]+)\s*=\s*"?([^"]*)"?\s*$', line)
            if match:
                values[match.group(1)] = match.group(2).strip()
        for key, value in values.items():
            if re.fullmatch(r"shader\d+", key):
                found.append(value)
        for name in filter(None, values.get("textures", "").split(";")):
            if name in values:
                found.append(values[name])
    else:
        for line in lines:
            include = re.match(r'\s*#include\s+"([^"]+)"', line)
            if include:
                found.append(include.group(1))
    return [os.path.normpath(os.path.join(here, name)) for name in found]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, out = (os.path.abspath(a) for a in sys.argv[1:])
    waiting = [os.path.join(source, p) for p in presets()]
    copied = set()
    while waiting:
        path = waiting.pop()
        if path in copied:
            continue
        if not os.path.isfile(path):
            sys.exit(f"shaders.py: {os.path.relpath(path, source)} is not in {source}")
        if os.path.commonpath([path, source]) != source:
            sys.exit(f"shaders.py: {path} is outside {source}")
        copied.add(path)
        if path.endswith((".slangp", ".slang", ".h", ".inc", ".glsl")):
            waiting.extend(uses(path))
    if os.path.isdir(out):
        shutil.rmtree(out)
    for path in sorted(copied):
        to = os.path.join(out, os.path.relpath(path, source))
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.copyfile(path, to)
    print(f"shaders.py: {len(presets())} presets, {len(copied)} files in {out}")


if __name__ == "__main__":
    main()
