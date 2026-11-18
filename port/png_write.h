// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Minimal PNG writer (RGB, zlib): screenshots, and Export pictures (pictures.h). Each row takes the filter that
// leaves the smallest sum of its bytes (as signed values), the PNG standard's suggestion; zlib at its best.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <zlib.h>

namespace png_write_detail {

inline uint8_t Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

} // namespace png_write_detail

// rgb: width * height pixels, three bytes each (red, green, blue), row after row. Empty on a failure.
inline std::vector<uint8_t> EncodePng(const uint8_t *rgb, uint32_t w, uint32_t h) {
    const size_t stride = static_cast<size_t>(w) * 3;
    std::vector<uint8_t> raw((stride + 1) * h), candidate(stride);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = rgb + y * stride, *up = y ? row - stride : nullptr;
        uint8_t *out = raw.data() + y * (stride + 1);
        uint64_t best = UINT64_MAX;
        for (uint8_t filter = 0; filter < 5; filter++) {
            uint64_t sum = 0;
            for (size_t i = 0; i < stride; i++) {
                const int a = i >= 3 ? row[i - 3] : 0, b = up ? up[i] : 0, c = (i >= 3 && up) ? up[i - 3] : 0;
                uint8_t v = row[i];
                switch (filter) {
                case 1: v = static_cast<uint8_t>(v - a); break;
                case 2: v = static_cast<uint8_t>(v - b); break;
                case 3: v = static_cast<uint8_t>(v - (a + b) / 2); break;
                case 4: v = static_cast<uint8_t>(v - png_write_detail::Paeth(a, b, c)); break;
                default: break;
                }
                candidate[i] = v;
                sum += static_cast<uint64_t>(std::abs(static_cast<int8_t>(v)));
            }
            if (sum < best) {
                best = sum;
                out[0] = filter;
                std::copy(candidate.begin(), candidate.end(), out + 1);
            }
        }
    }
    uLongf zSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zSize);
    if (compress2(z.data(), &zSize, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) {
        return {};
    }
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    auto put32 = [&](uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            png.push_back(static_cast<uint8_t>(v >> shift));
        }
    };
    auto chunk = [&](const char *type, const uint8_t *data, uint32_t size) {
        put32(size);
        png.insert(png.end(), type, type + 4);
        if (size) {
            png.insert(png.end(), data, data + size);
        }
        uLong crc = crc32(0, reinterpret_cast<const Bytef *>(type), 4);
        if (size) {
            crc = crc32(crc, data, size);
        }
        put32(static_cast<uint32_t>(crc));
    };
    const uint8_t ihdr[13] = {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16),
                              static_cast<uint8_t>(w >> 8),  static_cast<uint8_t>(w),
                              static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                              static_cast<uint8_t>(h >> 8),  static_cast<uint8_t>(h),
                              8, 2, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), static_cast<uint32_t>(zSize));
    chunk("IEND", nullptr, 0);
    return png;
}

// pixels: little-endian XBGR8888 (bytes R, G, B, X), as the Saturn software renderer produces.
inline void WritePng(const char *path, const uint32_t *pixels, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        rgb[i * 3] = static_cast<uint8_t>(pixels[i]);
        rgb[i * 3 + 1] = static_cast<uint8_t>(pixels[i] >> 8);
        rgb[i * 3 + 2] = static_cast<uint8_t>(pixels[i] >> 16);
    }
    const std::vector<uint8_t> png = EncodePng(rgb.data(), w, h);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
}
