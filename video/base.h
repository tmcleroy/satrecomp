// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Small things the video code is written with: integer names, bit fields, big-endian memory, colours.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace satrecomp::video {

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint8 = int8_t;
using sint16 = int16_t;
using sint32 = int32_t;
using sint64 = int64_t;

#if defined(__GNUC__) || defined(__clang__)
#define SV_INLINE inline __attribute__((always_inline))
#else
#define SV_INLINE __forceinline
#endif

// Bits lo..hi of a value
constexpr uint32 Bits(uint32 value, unsigned lo, unsigned hi) {
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
constexpr bool Bit(uint32 value, unsigned n) {
    return (value >> n) & 1u;
}
// The low `bits` bits of a value as a signed number
constexpr sint32 Signed(uint32 value, unsigned bits) {
    const uint32 sign = 1u << (bits - 1);
    value &= (sign << 1) - 1u;
    return static_cast<sint32>((value ^ sign) - sign);
}
constexpr sint64 Signed64(uint64 value, unsigned bits) {
    const uint64 sign = 1ull << (bits - 1);
    value &= (sign << 1) - 1ull;
    return static_cast<sint64>((value ^ sign) - sign);
}

SV_INLINE uint16 Be16(const uint8 *p) {
    return static_cast<uint16>((p[0] << 8) | p[1]);
}
SV_INLINE uint32 Be32(const uint8 *p) {
    return (static_cast<uint32>(p[0]) << 24) | (static_cast<uint32>(p[1]) << 16) | (static_cast<uint32>(p[2]) << 8) | p[3];
}

// A 15-bit colour as the Saturn stores it (red in the low five bits, bit 15 a flag), and a 24-bit one as the
// picture has it (red in the low byte)
union Color555 {
    uint16 u16;
    struct {
        uint16 r : 5;
        uint16 g : 5;
        uint16 b : 5;
        uint16 msb : 1;
    };
};
union Color888 {
    uint32 u32;
    struct {
        uint32 r : 8;
        uint32 g : 8;
        uint32 b : 8;
        uint32 pad : 7;
        uint32 msb : 1;
    };
};
// (five bits to eight as the hardware does it: shifted up, the low three bits zero)
constexpr uint32 To888(uint16 color) {
    return ((color & 31u) << 3) | (((color >> 5) & 31u) << 11) | (((color >> 10) & 31u) << 19) | ((color & 0x8000u) ? 0x80000000u : 0u);
}
SV_INLINE Color888 ConvertRGB555to888(Color555 color) {
    return Color888{.u32 = To888(color.u16)};
}

struct CoordS32 {
    sint32 cx, cy;
    sint32 &x() {
        return cx;
    }
    sint32 &y() {
        return cy;
    }
    const sint32 &x() const {
        return cx;
    }
    const sint32 &y() const {
        return cy;
    }
};

// Bit fields by position, as templates
namespace bit {
    template <unsigned N, typename T = sint32>
    constexpr T sign_extend(uint32 value) {
        return static_cast<T>(Signed(value, N));
    }
    template <unsigned Lo, unsigned Hi>
    constexpr uint32 extract(uint32 value) {
        return Bits(value, Lo, Hi);
    }
    template <unsigned Lo, unsigned Hi, typename T = sint32>
    constexpr T extract_signed(uint32 value) {
        return static_cast<T>(Signed(value >> Lo, Hi - Lo + 1));
    }
} // namespace bit

} // namespace satrecomp::video
