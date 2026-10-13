#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""SH-2 disassembler (the instruction set of the Saturn's SH7604).

As a module: disasm(opcode, pc) -> text.
As a tool:   sh2dis.py FILE BASE START [COUNT]   disassemble a memory dump loaded at BASE (hex), from START
             sh2dis.py --hex PC HEXBYTES          disassemble a block database entry
"""

import struct
import sys


def _s8(v):
    return v - 0x100 if v & 0x80 else v


def _s12(v):
    return v - 0x1000 if v & 0x800 else v


def disasm(op, pc=0):
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    d4 = op & 15
    d8 = op & 0xFF
    top = op >> 12
    low = op & 15
    rn, rm = "r%d" % n, "r%d" % m

    fixed = {0x0009: "nop", 0x000B: "rts", 0x002B: "rte", 0x0008: "clrt", 0x0018: "sett", 0x0019: "div0u",
             0x001B: "sleep", 0x0028: "clrmac"}
    if op in fixed:
        return fixed[op]

    if top == 0x0:
        t = {0x4: "mov.b %s,@(r0,%s)", 0x5: "mov.w %s,@(r0,%s)", 0x6: "mov.l %s,@(r0,%s)", 0x7: "mul.l %s,%s",
             0xC: "mov.b @(r0,%s),%s", 0xD: "mov.w @(r0,%s),%s", 0xE: "mov.l @(r0,%s),%s", 0xF: "mac.l @%s+,@%s+"}
        if low in t:
            return t[low] % (rm, rn)
        t = {0x02: "stc sr,%s", 0x12: "stc gbr,%s", 0x22: "stc vbr,%s", 0x03: "bsrf %s", 0x23: "braf %s",
             0x0A: "sts mach,%s", 0x1A: "sts macl,%s", 0x2A: "sts pr,%s", 0x29: "movt %s"}
        if d8 in t:
            return t[d8] % rn
    elif top == 0x1:
        return "mov.l %s,@(%d,%s)" % (rm, d4 * 4, rn)
    elif top == 0x2:
        t = {0x0: "mov.b %s,@%s", 0x1: "mov.w %s,@%s", 0x2: "mov.l %s,@%s", 0x4: "mov.b %s,@-%s",
             0x5: "mov.w %s,@-%s", 0x6: "mov.l %s,@-%s", 0x7: "div0s %s,%s", 0x8: "tst %s,%s", 0x9: "and %s,%s",
             0xA: "xor %s,%s", 0xB: "or %s,%s", 0xC: "cmp/str %s,%s", 0xD: "xtrct %s,%s", 0xE: "mulu.w %s,%s",
             0xF: "muls.w %s,%s"}
        if low in t:
            return t[low] % (rm, rn)
    elif top == 0x3:
        t = {0x0: "cmp/eq", 0x2: "cmp/hs", 0x3: "cmp/ge", 0x4: "div1", 0x5: "dmulu.l", 0x6: "cmp/hi", 0x7: "cmp/gt",
             0x8: "sub", 0xA: "subc", 0xB: "subv", 0xC: "add", 0xD: "dmuls.l", 0xE: "addc", 0xF: "addv"}
        if low in t:
            return "%s %s,%s" % (t[low], rm, rn)
    elif top == 0x4:
        if low == 0xF:
            return "mac.w @%s+,@%s+" % (rm, rn)
        t = {0x00: "shll %s", 0x10: "dt %s", 0x20: "shal %s", 0x01: "shlr %s", 0x11: "cmp/pz %s", 0x21: "shar %s",
             0x02: "sts.l mach,@-%s", 0x12: "sts.l macl,@-%s", 0x22: "sts.l pr,@-%s", 0x03: "stc.l sr,@-%s",
             0x13: "stc.l gbr,@-%s", 0x23: "stc.l vbr,@-%s", 0x04: "rotl %s", 0x24: "rotcl %s", 0x05: "rotr %s",
             0x15: "cmp/pl %s", 0x25: "rotcr %s", 0x06: "lds.l @%s+,mach", 0x16: "lds.l @%s+,macl",
             0x26: "lds.l @%s+,pr", 0x07: "ldc.l @%s+,sr", 0x17: "ldc.l @%s+,gbr", 0x27: "ldc.l @%s+,vbr",
             0x08: "shll2 %s", 0x18: "shll8 %s", 0x28: "shll16 %s", 0x09: "shlr2 %s", 0x19: "shlr8 %s",
             0x29: "shlr16 %s", 0x0A: "lds %s,mach", 0x1A: "lds %s,macl", 0x2A: "lds %s,pr", 0x0B: "jsr @%s",
             0x1B: "tas.b @%s", 0x2B: "jmp @%s", 0x0E: "ldc %s,sr", 0x1E: "ldc %s,gbr", 0x2E: "ldc %s,vbr"}
        if d8 in t:
            return t[d8] % rn
    elif top == 0x5:
        return "mov.l @(%d,%s),%s" % (d4 * 4, rm, rn)
    elif top == 0x6:
        t = {0x0: "mov.b @%s,%s", 0x1: "mov.w @%s,%s", 0x2: "mov.l @%s,%s", 0x3: "mov %s,%s", 0x4: "mov.b @%s+,%s",
             0x5: "mov.w @%s+,%s", 0x6: "mov.l @%s+,%s", 0x7: "not %s,%s", 0x8: "swap.b %s,%s", 0x9: "swap.w %s,%s",
             0xA: "negc %s,%s", 0xB: "neg %s,%s", 0xC: "extu.b %s,%s", 0xD: "extu.w %s,%s", 0xE: "exts.b %s,%s",
             0xF: "exts.w %s,%s"}
        return t[low] % (rm, rn)
    elif top == 0x7:
        return "add #%d,%s" % (_s8(d8), rn)
    elif top == 0x8:
        t = {0x0: "mov.b r0,@(%d,%s)" % (d4, rm), 0x1: "mov.w r0,@(%d,%s)" % (d4 * 2, rm),
             0x4: "mov.b @(%d,%s),r0" % (d4, rm), 0x5: "mov.w @(%d,%s),r0" % (d4 * 2, rm),
             0x8: "cmp/eq #%d,r0" % _s8(d8)}
        if n in t:
            return t[n]
        b = {0x9: "bt", 0xB: "bf", 0xD: "bt/s", 0xF: "bf/s"}
        if n in b:
            return "%s 0x%08X" % (b[n], (pc + 4 + _s8(d8) * 2) & 0xFFFFFFFF)
    elif top == 0x9:
        return "mov.w @(0x%08X),%s" % ((pc + 4 + d8 * 2) & 0xFFFFFFFF, rn)
    elif top == 0xA:
        return "bra 0x%08X" % ((pc + 4 + _s12(op & 0xFFF) * 2) & 0xFFFFFFFF)
    elif top == 0xB:
        return "bsr 0x%08X" % ((pc + 4 + _s12(op & 0xFFF) * 2) & 0xFFFFFFFF)
    elif top == 0xC:
        t = {0x0: "mov.b r0,@(%d,gbr)" % d8, 0x1: "mov.w r0,@(%d,gbr)" % (d8 * 2), 0x2: "mov.l r0,@(%d,gbr)" % (d8 * 4),
             0x3: "trapa #%d" % d8, 0x4: "mov.b @(%d,gbr),r0" % d8, 0x5: "mov.w @(%d,gbr),r0" % (d8 * 2),
             0x6: "mov.l @(%d,gbr),r0" % (d8 * 4), 0x7: "mova @(0x%08X),r0" % (((pc & ~3) + 4 + d8 * 4) & 0xFFFFFFFF),
             0x8: "tst #%d,r0" % d8, 0x9: "and #%d,r0" % d8, 0xA: "xor #%d,r0" % d8, 0xB: "or #%d,r0" % d8,
             0xC: "tst.b #%d,@(r0,gbr)" % d8, 0xD: "and.b #%d,@(r0,gbr)" % d8, 0xE: "xor.b #%d,@(r0,gbr)" % d8,
             0xF: "or.b #%d,@(r0,gbr)" % d8}
        return t[n]
    elif top == 0xD:
        return "mov.l @(0x%08X),%s" % (((pc & ~3) + 4 + d8 * 4) & 0xFFFFFFFF, rn)
    elif top == 0xE:
        return "mov #%d,%s" % (_s8(d8), rn)
    return ".word 0x%04X" % op


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "--hex":
        pc = int(a[1], 16)
        code = bytes.fromhex(a[2])
        for i in range(0, len(code), 2):
            op = struct.unpack(">H", code[i:i + 2])[0]
            print("%08X  %04X  %s" % (pc + i, op, disasm(op, pc + i)))
        return
    if len(a) < 3:
        sys.exit(__doc__)
    data = open(a[0], "rb").read()
    base = int(a[1], 16)
    start = int(a[2], 16)
    count = int(a[3]) if len(a) > 3 else 32
    for i in range(count):
        pc = start + i * 2
        off = pc - base
        op = struct.unpack(">H", data[off:off + 2])[0]
        lit = ""
        if op >> 12 == 0xD:
            la = ((pc & ~3) + 4 + (op & 0xFF) * 4) - base
            lit = "   ; =0x%08X" % struct.unpack(">I", data[la:la + 4])[0]
        elif op >> 12 == 0x9:
            la = (pc + 4 + (op & 0xFF) * 2) - base
            lit = "   ; =0x%04X" % struct.unpack(">H", data[la:la + 2])[0]
        print("%08X  %04X  %s%s" % (pc, op, disasm(op, pc), lit))


if __name__ == "__main__":
    main()
