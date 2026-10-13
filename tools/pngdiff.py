#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""Compare screenshots of two runs (--shot-every), e.g. the GPU renderer's against the software renderer's.

  pngdiff.py DIR_A DIR_B [--sheet OUT.png] [--field]

Prints, for every picture both directories have, how many pixels differ by more than a small amount, and the
total. --field: of an interlaced picture, only the rows of the field that differs least (the software
renderer's picture keeps the field before in the other rows; the GPU renderer's does not). --sheet writes the
pairs side by side, shrunk, as one picture. Pure Python (zlib only).
"""
import struct
import sys
import zlib
from pathlib import Path


def read_png(path):
    data = Path(path).read_bytes()
    pos = 8
    width = height = 0
    channels = 4
    raw = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            channels = {2: 3, 6: 4}[color]
            assert depth == 8
        elif kind == b"IDAT":
            raw += body
        pos += 12 + length
    raw = zlib.decompress(raw)
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    at = 0
    for _ in range(height):
        f = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, channels, rows


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def rgb(image, x, y):
    _, _, ch, rows = image
    return rows[y][x * ch:x * ch + 3]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sheet = sys.argv[sys.argv.index("--sheet") + 1] if "--sheet" in sys.argv else None
    if sheet:
        args.remove(sheet)
    a_dir, b_dir = Path(args[0]), Path(args[1])
    names = sorted(p.name for p in a_dir.glob("*.png") if (b_dir / p.name).exists())
    total = differing = 0
    sheet_rows = []
    for name in names:
        a, b = read_png(a_dir / name), read_png(b_dir / name)
        if a[:2] != b[:2]:
            print(f"{name}: sizes differ {a[:2]} {b[:2]}")
        else:
            w, h = a[:2]
            counts = [0, 0]
            for y in range(h):
                ra, rb, ca, cb = a[3][y], b[3][y], a[2], b[2]
                n = 0
                for x in range(w):
                    pa, pb = ra[x * ca:x * ca + 3], rb[x * cb:x * cb + 3]
                    if pa != pb and max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > 24:
                        n += 1
                counts[y & 1] += n
            if "--field" in sys.argv:
                n, pixels = min(counts), w * (h // 2)
            else:
                n, pixels = sum(counts), w * h
            total += pixels
            differing += n
            print(f"{name}: {100.0 * n / pixels:6.2f}% of pixels differ")
        if sheet:
            # the pair side by side at half size
            for y in range(0, min(a[1], b[1]) - 1, 2):
                row = bytearray()
                for image in (a, b):
                    for x in range(0, image[0] - 1, 2):
                        row += rgb(image, x, y)
                    row += b"\x00\x00\x00" * 4
                sheet_rows.append(row)
            sheet_rows.append(bytearray(len(sheet_rows[-1])))
    if total:
        print(f"all: {100.0 * differing / total:.2f}% of pixels differ")
    if sheet and sheet_rows:
        width = max(len(r) for r in sheet_rows) // 3
        rows = [r + bytearray(width * 3 - len(r)) for r in sheet_rows]
        write_png(sheet, width, len(rows), rows)


if __name__ == "__main__":
    main()
