// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The SH-2 instruction set: every instruction, and the bits of an opcode that say it is that one.
//
// (generated once from the instruction encodings; the list is the SH7604 manual's)
#pragma once

#include <cstdint>

namespace satrecomp::machine {

// clang-format off
enum class Op : uint8_t {
    NOP, SLEEP, MOV_R, MOVB_L, MOVW_L, MOVL_L, MOVB_L0, MOVW_L0, MOVL_L0, MOVB_L4, MOVW_L4, MOVL_L4, MOVB_LG,
    MOVW_LG, MOVL_LG, MOVB_M, MOVW_M, MOVL_M, MOVB_P, MOVW_P, MOVL_P, MOVB_S, MOVW_S, MOVL_S, MOVB_S0, MOVW_S0,
    MOVL_S0, MOVB_S4, MOVW_S4, MOVL_S4, MOVB_SG, MOVW_SG, MOVL_SG, MOV_I, MOVW_I, MOVL_I, MOVA, MOVT, CLRT, SETT,
    EXTUB, EXTUW, EXTSB, EXTSW, SWAPB, SWAPW, XTRCT, LDC_GBR_R, LDC_SR_R, LDC_VBR_R, LDS_MACH_R, LDS_MACL_R,
    LDS_PR_R, STC_GBR_R, STC_SR_R, STC_VBR_R, STS_MACH_R, STS_MACL_R, STS_PR_R, LDC_GBR_M, LDC_SR_M, LDC_VBR_M,
    LDS_MACH_M, LDS_MACL_M, LDS_PR_M, STC_GBR_M, STC_SR_M, STC_VBR_M, STS_MACH_M, STS_MACL_M, STS_PR_M, ADD, ADD_I,
    ADDC, ADDV, AND_R, AND_I, AND_M, NEG, NEGC, NOT, OR_R, OR_I, OR_M, ROTCL, ROTCR, ROTL, ROTR, SHAL, SHAR, SHLL,
    SHLL2, SHLL8, SHLL16, SHLR, SHLR2, SHLR8, SHLR16, SUB, SUBC, SUBV, XOR_R, XOR_I, XOR_M, DT, CLRMAC, MACW, MACL,
    MUL, MULS, MULU, DMULS, DMULU, DIV0S, DIV0U, DIV1, CMP_EQ_I, CMP_EQ_R, CMP_GE, CMP_GT, CMP_HI, CMP_HS, CMP_PL,
    CMP_PZ, CMP_STR, TAS, TST_R, TST_I, TST_M, BF, BFS, BT, BTS, BRA, BRAF, BSR, BSRF, JMP, JSR, TRAPA, RTE, RTS,
    Illegal
};

struct OpPattern {
    Op op;
    uint16_t mask, match;
};
inline constexpr OpPattern kOpPatterns[] = {
    {Op::NOP, 0xFFFF, 0x0009}, {Op::SLEEP, 0xFFFF, 0x001B}, {Op::MOV_R, 0xF00F, 0x6003},
    {Op::MOVB_L, 0xF00F, 0x6000}, {Op::MOVW_L, 0xF00F, 0x6001}, {Op::MOVL_L, 0xF00F, 0x6002},
    {Op::MOVB_L0, 0xF00F, 0x000C}, {Op::MOVW_L0, 0xF00F, 0x000D}, {Op::MOVL_L0, 0xF00F, 0x000E},
    {Op::MOVB_L4, 0xFF00, 0x8400}, {Op::MOVW_L4, 0xFF00, 0x8500}, {Op::MOVL_L4, 0xF000, 0x5000},
    {Op::MOVB_LG, 0xFF00, 0xC400}, {Op::MOVW_LG, 0xFF00, 0xC500}, {Op::MOVL_LG, 0xFF00, 0xC600},
    {Op::MOVB_M, 0xF00F, 0x2004}, {Op::MOVW_M, 0xF00F, 0x2005}, {Op::MOVL_M, 0xF00F, 0x2006},
    {Op::MOVB_P, 0xF00F, 0x6004}, {Op::MOVW_P, 0xF00F, 0x6005}, {Op::MOVL_P, 0xF00F, 0x6006},
    {Op::MOVB_S, 0xF00F, 0x2000}, {Op::MOVW_S, 0xF00F, 0x2001}, {Op::MOVL_S, 0xF00F, 0x2002},
    {Op::MOVB_S0, 0xF00F, 0x0004}, {Op::MOVW_S0, 0xF00F, 0x0005}, {Op::MOVL_S0, 0xF00F, 0x0006},
    {Op::MOVB_S4, 0xFF00, 0x8000}, {Op::MOVW_S4, 0xFF00, 0x8100}, {Op::MOVL_S4, 0xF000, 0x1000},
    {Op::MOVB_SG, 0xFF00, 0xC000}, {Op::MOVW_SG, 0xFF00, 0xC100}, {Op::MOVL_SG, 0xFF00, 0xC200},
    {Op::MOV_I, 0xF000, 0xE000}, {Op::MOVW_I, 0xF000, 0x9000}, {Op::MOVL_I, 0xF000, 0xD000},
    {Op::MOVA, 0xFF00, 0xC700}, {Op::MOVT, 0xF0FF, 0x0029}, {Op::CLRT, 0xFFFF, 0x0008}, {Op::SETT, 0xFFFF, 0x0018},
    {Op::EXTUB, 0xF00F, 0x600C}, {Op::EXTUW, 0xF00F, 0x600D}, {Op::EXTSB, 0xF00F, 0x600E},
    {Op::EXTSW, 0xF00F, 0x600F}, {Op::SWAPB, 0xF00F, 0x6008}, {Op::SWAPW, 0xF00F, 0x6009},
    {Op::XTRCT, 0xF00F, 0x200D}, {Op::LDC_GBR_R, 0xF0FF, 0x401E}, {Op::LDC_SR_R, 0xF0FF, 0x400E},
    {Op::LDC_VBR_R, 0xF0FF, 0x402E}, {Op::LDS_MACH_R, 0xF0FF, 0x400A}, {Op::LDS_MACL_R, 0xF0FF, 0x401A},
    {Op::LDS_PR_R, 0xF0FF, 0x402A}, {Op::STC_GBR_R, 0xF0FF, 0x0012}, {Op::STC_SR_R, 0xF0FF, 0x0002},
    {Op::STC_VBR_R, 0xF0FF, 0x0022}, {Op::STS_MACH_R, 0xF0FF, 0x000A}, {Op::STS_MACL_R, 0xF0FF, 0x001A},
    {Op::STS_PR_R, 0xF0FF, 0x002A}, {Op::LDC_GBR_M, 0xF0FF, 0x4017}, {Op::LDC_SR_M, 0xF0FF, 0x4007},
    {Op::LDC_VBR_M, 0xF0FF, 0x4027}, {Op::LDS_MACH_M, 0xF0FF, 0x4006}, {Op::LDS_MACL_M, 0xF0FF, 0x4016},
    {Op::LDS_PR_M, 0xF0FF, 0x4026}, {Op::STC_GBR_M, 0xF0FF, 0x4013}, {Op::STC_SR_M, 0xF0FF, 0x4003},
    {Op::STC_VBR_M, 0xF0FF, 0x4023}, {Op::STS_MACH_M, 0xF0FF, 0x4002}, {Op::STS_MACL_M, 0xF0FF, 0x4012},
    {Op::STS_PR_M, 0xF0FF, 0x4022}, {Op::ADD, 0xF00F, 0x300C}, {Op::ADD_I, 0xF000, 0x7000},
    {Op::ADDC, 0xF00F, 0x300E}, {Op::ADDV, 0xF00F, 0x300F}, {Op::AND_R, 0xF00F, 0x2009},
    {Op::AND_I, 0xFF00, 0xC900}, {Op::AND_M, 0xFF00, 0xCD00}, {Op::NEG, 0xF00F, 0x600B}, {Op::NEGC, 0xF00F, 0x600A},
    {Op::NOT, 0xF00F, 0x6007}, {Op::OR_R, 0xF00F, 0x200B}, {Op::OR_I, 0xFF00, 0xCB00}, {Op::OR_M, 0xFF00, 0xCF00},
    {Op::ROTCL, 0xF0FF, 0x4024}, {Op::ROTCR, 0xF0FF, 0x4025}, {Op::ROTL, 0xF0FF, 0x4004},
    {Op::ROTR, 0xF0FF, 0x4005}, {Op::SHAL, 0xF0FF, 0x4020}, {Op::SHAR, 0xF0FF, 0x4021}, {Op::SHLL, 0xF0FF, 0x4000},
    {Op::SHLL2, 0xF0FF, 0x4008}, {Op::SHLL8, 0xF0FF, 0x4018}, {Op::SHLL16, 0xF0FF, 0x4028},
    {Op::SHLR, 0xF0FF, 0x4001}, {Op::SHLR2, 0xF0FF, 0x4009}, {Op::SHLR8, 0xF0FF, 0x4019},
    {Op::SHLR16, 0xF0FF, 0x4029}, {Op::SUB, 0xF00F, 0x3008}, {Op::SUBC, 0xF00F, 0x300A}, {Op::SUBV, 0xF00F, 0x300B},
    {Op::XOR_R, 0xF00F, 0x200A}, {Op::XOR_I, 0xFF00, 0xCA00}, {Op::XOR_M, 0xFF00, 0xCE00}, {Op::DT, 0xF0FF, 0x4010},
    {Op::CLRMAC, 0xFFFF, 0x0028}, {Op::MACW, 0xF00F, 0x400F}, {Op::MACL, 0xF00F, 0x000F}, {Op::MUL, 0xF00F, 0x0007},
    {Op::MULS, 0xF00F, 0x200F}, {Op::MULU, 0xF00F, 0x200E}, {Op::DMULS, 0xF00F, 0x300D},
    {Op::DMULU, 0xF00F, 0x3005}, {Op::DIV0S, 0xF00F, 0x2007}, {Op::DIV0U, 0xFFFF, 0x0019},
    {Op::DIV1, 0xF00F, 0x3004}, {Op::CMP_EQ_I, 0xFF00, 0x8800}, {Op::CMP_EQ_R, 0xF00F, 0x3000},
    {Op::CMP_GE, 0xF00F, 0x3003}, {Op::CMP_GT, 0xF00F, 0x3007}, {Op::CMP_HI, 0xF00F, 0x3006},
    {Op::CMP_HS, 0xF00F, 0x3002}, {Op::CMP_PL, 0xF0FF, 0x4015}, {Op::CMP_PZ, 0xF0FF, 0x4011},
    {Op::CMP_STR, 0xF00F, 0x200C}, {Op::TAS, 0xF0FF, 0x401B}, {Op::TST_R, 0xF00F, 0x2008},
    {Op::TST_I, 0xFF00, 0xC800}, {Op::TST_M, 0xFF00, 0xCC00}, {Op::BF, 0xFF00, 0x8B00}, {Op::BFS, 0xFF00, 0x8F00},
    {Op::BT, 0xFF00, 0x8900}, {Op::BTS, 0xFF00, 0x8D00}, {Op::BRA, 0xF000, 0xA000}, {Op::BRAF, 0xF0FF, 0x0023},
    {Op::BSR, 0xF000, 0xB000}, {Op::BSRF, 0xF0FF, 0x0003}, {Op::JMP, 0xF0FF, 0x402B}, {Op::JSR, 0xF0FF, 0x400B},
    {Op::TRAPA, 0xFF00, 0xC300}, {Op::RTE, 0xFFFF, 0x002B}, {Op::RTS, 0xFFFF, 0x000B},
};
// clang-format on

} // namespace satrecomp::machine
