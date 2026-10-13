# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""SH-2 to C: satrecomp's own translation of each instruction (the "native" code path).

emit(name, op, ...) returns the C statements for one instruction, written against the macros of
runtime/block_native.h. Registers are R[n]; the other CPU state is reached through `c`. Nothing here calls the
interpreter.

What an instruction does is checked against the interpreter by running both (tools/verify.sh native).
"""

WB_NONE = 0xFF  # no register being written back by the previous instruction
WB_PR = 0x10


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s12(v):
    return v - 0x1000 if v & 0x800 else v


class Instr:
    """One translated instruction."""

    def __init__(self, lines, wb=WB_NONE, mem=False, clears_allow=False, sr=False):
        self.lines = lines          # C statements
        self.wb = wb                # register whose load completes during the next instruction (m_wbReg)
        self.mem = mem              # accesses memory: hardware may have raised an interrupt
        self.clears_allow = clears_allow  # holds off interrupts for one instruction
        self.sr = sr                # writes SR: an interrupt may have become due


def emit(name, op, pcx, on_wait, in_slot=False, fallback=None):
    """name: opcode type; op: opcode; pcx: C expression for this instruction's address as PC-relative
    addressing sees it; on_wait: C statement that leaves the block when the bus makes the access wait;
    fallback: C call text of the interpreter's handler (for the instructions not translated here).
    Returns an Instr, or None for an instruction that ends a block (handled by the caller)."""
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    d4 = op & 15
    d8 = op & 0xFF
    imm = s8(d8)
    Rn, Rm, R0 = "R[%d]" % n, "R[%d]" % m, "R[0]"
    sx8 = "(uint32)(sint32)(sint8)v"
    sx16 = "(uint32)(sint32)(sint16)v"

    def load(bits, addr, dst, value=None, wait=True, wb=WB_NONE, post=None):
        value = value or {8: sx8, 16: sx16, 32: "v"}[bits]
        rd = "SN_RD%d(v, %s, %s)" % (bits, addr, on_wait) if wait and bits != 8 else "SN_RD%dN(v, %s)" % (bits, addr)
        lines = [rd, "%s = %s;" % (dst, value)]
        if post:
            lines.append(post)
        return Instr(lines, wb=wb, mem=True)

    def store(bits, addr, value, wait=True, post=None, pre=None):
        wr = "SN_WR%d(%s, %s, %s)" % (bits, addr, value, on_wait) if wait and bits != 8 else \
            "SN_WR%dN(%s, %s)" % (bits, addr, value)
        lines = ([pre] if pre else []) + [wr] + ([post] if post else [])
        return Instr(lines, mem=True)

    def reg(*lines, **kw):
        return Instr(list(lines), **kw)

    size = {"B": 8, "W": 16, "L": 32}
    shift = {"B": 0, "W": 1, "L": 2}

    # --- data transfer ---
    if name == "NOP":
        return reg()
    if name == "MOV_R":
        return reg("%s = %s;" % (Rn, Rm))
    if name in ("MOVB_L", "MOVW_L", "MOVL_L"):
        return load(size[name[3]], Rm, Rn, wb=n)
    if name in ("MOVB_L0", "MOVW_L0", "MOVL_L0"):
        return load(size[name[3]], "%s + R[0]" % Rm, Rn, wb=n)
    if name in ("MOVB_L4", "MOVW_L4"):
        return load(size[name[3]], "%s + %du" % (Rm, d4 << shift[name[3]]), R0, wb=0)
    if name == "MOVL_L4":
        return load(32, "%s + %du" % (Rm, d4 * 4), Rn, wb=n)
    if name in ("MOVB_LG", "MOVW_LG", "MOVL_LG"):
        return load(size[name[3]], "c.GBR + %du" % (d8 << shift[name[3]]), R0, wb=0)
    if name in ("MOVB_M", "MOVW_M", "MOVL_M"):
        bits = size[name[3]]
        # the address goes into Rn only once the write went through (it may have had to wait)
        return store(bits, "%s - %du" % (Rn, bits // 8), Rm, post="%s -= %du;" % (Rn, bits // 8))
    if name in ("MOVB_P", "MOVW_P", "MOVL_P"):
        bits = size[name[3]]
        return load(bits, Rm, Rn, wb=n, post=None if n == m else "%s += %du;" % (Rm, bits // 8))
    if name in ("MOVB_S", "MOVW_S", "MOVL_S"):
        return store(size[name[3]], Rn, Rm)
    if name in ("MOVB_S0", "MOVW_S0", "MOVL_S0"):
        return store(size[name[3]], "%s + R[0]" % Rn, Rm)
    if name in ("MOVB_S4", "MOVW_S4"):
        return store(size[name[3]], "%s + %du" % (Rm, d4 << shift[name[3]]), R0)  # (register field is bits 4-7)
    if name == "MOVL_S4":
        return store(32, "%s + %du" % (Rn, d4 * 4), Rm)
    if name in ("MOVB_SG", "MOVW_SG", "MOVL_SG"):
        return store(size[name[3]], "c.GBR + %du" % (d8 << shift[name[3]]), R0)
    if name == "MOV_I":
        return reg("%s = 0x%08Xu;" % (Rn, imm & 0xFFFFFFFF))
    if name == "MOVW_I":
        return load(16, "%s + %du" % (pcx, d8 * 2 + 4), Rn, wait=False, wb=n)
    if name == "MOVL_I":
        return load(32, "(%s & ~3u) + %du" % (pcx, d8 * 4 + 4), Rn, wait=False, wb=n)
    if name == "MOVA":
        return reg("R[0] = (%s & ~3u) + %du;" % (pcx, d8 * 4 + 4))
    if name == "MOVT":
        return reg("%s = c.SR.T;" % Rn)
    if name == "CLRT":
        return reg("c.SR.T = 0;")
    if name == "SETT":
        return reg("c.SR.T = 1;")
    if name == "EXTUB":
        return reg("%s = %s & 0xFFu;" % (Rn, Rm))
    if name == "EXTUW":
        return reg("%s = %s & 0xFFFFu;" % (Rn, Rm))
    if name == "EXTSB":
        return reg("%s = (uint32)(sint32)(sint8)%s;" % (Rn, Rm))
    if name == "EXTSW":
        return reg("%s = (uint32)(sint32)(sint16)%s;" % (Rn, Rm))
    if name == "SWAPB":
        return reg("%s = (%s & 0xFFFF0000u) | ((%s & 0xFFu) << 8) | ((%s >> 8) & 0xFFu);" % (Rn, Rm, Rm, Rm))
    if name == "SWAPW":
        return reg("%s = (%s << 16) | (%s >> 16);" % (Rn, Rm, Rm))
    if name == "XTRCT":
        return reg("%s = (%s >> 16) | (%s << 16);" % (Rn, Rn, Rm))

    # --- control and system registers (these hold off interrupts for one instruction) ---
    # (register field of the "m" forms is bits 8-11, the same bits as n)
    ctl = {"GBR": "c.GBR", "VBR": "c.VBR", "MACH": "c.MAC.H", "MACL": "c.MAC.L", "PR": "c.PR", "SR": "c.SR.u32"}
    set_sr = "c.SR.u32 = %%s & 0x3F3u; c.m_intrFlags.pending = %s;" % (
        "false" if in_slot else "c.INTC.pending.level > c.SR.ILevel")
    parts = name.split("_")
    if parts[0] in ("LDC", "LDS", "STC", "STS") and len(parts) == 3:
        what, form = parts[1], parts[2]
        if parts[0] in ("LDC", "LDS"):
            if form == "R":
                if what == "SR":
                    return reg(set_sr % Rn, clears_allow=True, sr=True)
                return reg("%s = %s;" % (ctl[what], Rn), clears_allow=True)
            ins = Instr(["SN_RD32N(v, %s)" % Rn,
                         (set_sr % "v") if what == "SR" else "%s = v;" % ctl[what],
                         "%s += 4u;" % Rn], mem=True, clears_allow=True, sr=what == "SR")
            ins.wb = WB_PR if what == "PR" else WB_NONE
            return ins
        if form == "R":
            return reg("%s = %s;" % (Rn, ctl[what]), clears_allow=True, wb=n if what in ("MACH", "MACL") else WB_NONE)
        return Instr(["%s -= 4u;" % Rn, "SN_WR32N(%s, %s)" % (Rn, ctl[what])], mem=True, clears_allow=True)

    # --- arithmetic and logic ---
    binop = {"ADD": "+=", "SUB": "-=", "AND_R": "&=", "OR_R": "|=", "XOR_R": "^="}
    if name in binop:
        return reg("%s %s %s;" % (Rn, binop[name], Rm))
    if name == "ADD_I":
        return reg("%s += 0x%08Xu;" % (Rn, imm & 0xFFFFFFFF))
    if name == "ADDC":
        return reg("const uint32 t1 = %s + %s; const uint32 t0 = %s;" % (Rn, Rm, Rn),
                   "%s = t1 + c.SR.T;" % Rn, "c.SR.T = (t0 > t1) || (t1 > %s);" % Rn)
    if name == "SUBC":
        return reg("const uint32 t1 = %s - %s; const uint32 t0 = %s;" % (Rn, Rm, Rn),
                   "%s = t1 - c.SR.T;" % Rn, "c.SR.T = (t0 < t1) || (t1 < %s);" % Rn)
    if name in ("ADDV", "SUBV"):
        return reg("const bool dst = (sint32)%s < 0; const bool src = (sint32)%s < 0;" % (Rn, Rm),
                   "%s %s= %s;" % (Rn, "+" if name == "ADDV" else "-", Rm),
                   "c.SR.T = (src %s dst) & (((sint32)%s < 0) ^ dst);" % ("==" if name == "ADDV" else "!=", Rn))
    imm_op = {"AND_I": "&=", "OR_I": "|=", "XOR_I": "^="}
    if name in imm_op:
        return reg("R[0] %s 0x%02Xu;" % (imm_op[name], d8))
    mem_op = {"AND_M": "&", "OR_M": "|", "XOR_M": "^"}
    if name in mem_op:
        return Instr(["const uint32 a = c.GBR + R[0];", "SN_RD8N(v, a)",
                      "SN_WR8N(a, (uint8)(v %s 0x%02Xu))" % (mem_op[name], d8)], mem=True)
    if name == "NEG":
        return reg("%s = 0u - %s;" % (Rn, Rm))
    if name == "NEGC":
        return reg("const uint32 t = 0u - %s;" % Rm, "%s = t - c.SR.T;" % Rn, "c.SR.T = (0 < t) || (t < %s);" % Rn)
    if name == "NOT":
        return reg("%s = ~%s;" % (Rn, Rm))
    if name == "ROTCL":
        return reg("const uint32 t = %s >> 31;" % Rn, "%s = (%s << 1) | c.SR.T;" % (Rn, Rn), "c.SR.T = t;")
    if name == "ROTCR":
        return reg("const uint32 t = %s & 1u;" % Rn, "%s = (%s >> 1) | ((uint32)c.SR.T << 31);" % (Rn, Rn), "c.SR.T = t;")
    if name == "ROTL":
        return reg("c.SR.T = %s >> 31;" % Rn, "%s = (%s << 1) | c.SR.T;" % (Rn, Rn))
    if name == "ROTR":
        return reg("c.SR.T = %s & 1u;" % Rn, "%s = (%s >> 1) | ((uint32)c.SR.T << 31);" % (Rn, Rn))
    if name in ("SHAL", "SHLL"):
        return reg("c.SR.T = %s >> 31;" % Rn, "%s <<= 1;" % Rn)
    if name == "SHAR":
        return reg("c.SR.T = %s & 1u;" % Rn, "%s = (uint32)((sint32)%s >> 1);" % (Rn, Rn))
    if name == "SHLR":
        return reg("c.SR.T = %s & 1u;" % Rn, "%s >>= 1;" % Rn)
    shifts = {"SHLL2": "<<= 2", "SHLL8": "<<= 8", "SHLL16": "<<= 16", "SHLR2": ">>= 2", "SHLR8": ">>= 8",
              "SHLR16": ">>= 16"}
    if name in shifts:
        return reg("%s %s;" % (Rn, shifts[name]))
    if name == "DT":
        return reg("%s -= 1u;" % Rn, "c.SR.T = %s == 0;" % Rn)

    # --- multiply and divide ---
    if name == "CLRMAC":
        return reg("c.MAC.u64 = 0;")
    if name == "MUL":
        return reg("c.MAC.L = %s * %s;" % (Rm, Rn))
    if name == "MULS":
        return reg("c.MAC.L = (uint32)((sint32)(sint16)%s * (sint32)(sint16)%s);" % (Rm, Rn))
    if name == "MULU":
        return reg("c.MAC.L = (uint32)(uint16)%s * (uint32)(uint16)%s;" % (Rm, Rn))
    if name == "DMULS":
        return reg("c.MAC.u64 = (uint64)((sint64)(sint32)%s * (sint64)(sint32)%s);" % (Rm, Rn))
    if name == "DMULU":
        return reg("c.MAC.u64 = (uint64)%s * (uint64)%s;" % (Rm, Rn))
    if name == "DIV0S":
        return reg("c.SR.M = (sint32)%s < 0;" % Rm, "c.SR.Q = (sint32)%s < 0;" % Rn, "c.SR.T = c.SR.M != c.SR.Q;")
    if name == "DIV0U":
        return reg("c.SR.M = 0; c.SR.Q = 0; c.SR.T = 0;")
    if name == "DIV1":
        return reg("const bool oldQ = c.SR.Q; const bool M = c.SR.M;",
                   "bool Q = (sint32)%s < 0;" % Rn,
                   "%s = (%s << 1) | c.SR.T;" % (Rn, Rn),
                   "const uint32 prev = %s;" % Rn,
                   "if (oldQ == M) { %s -= %s; } else { %s += %s; }" % (Rn, Rm, Rn, Rm),
                   "if (oldQ) { Q ^= M ? (%s <= prev) : (%s < prev); } else { Q ^= M ? (%s >= prev) : (%s > prev); }"
                   % (Rn, Rn, Rn, Rn),
                   "c.SR.T = Q == M; c.SR.Q = Q;")

    # --- compare and test ---
    if name == "CMP_EQ_I":
        return reg("c.SR.T = R[0] == 0x%08Xu;" % (imm & 0xFFFFFFFF))
    cmp = {"CMP_EQ_R": ("", "=="), "CMP_GE": ("(sint32)", ">="), "CMP_GT": ("(sint32)", ">"), "CMP_HI": ("", ">"),
           "CMP_HS": ("", ">=")}
    if name in cmp:
        cast, rel = cmp[name]
        return reg("c.SR.T = %s%s %s %s%s;" % (cast, Rn, rel, cast, Rm))
    if name == "CMP_PL":
        return reg("c.SR.T = (sint32)%s > 0;" % Rn)
    if name == "CMP_PZ":
        return reg("c.SR.T = (sint32)%s >= 0;" % Rn)
    if name == "CMP_STR":
        return reg("const uint32 t = %s ^ %s;" % (Rm, Rn),
                   "c.SR.T = !((t >> 24) && ((t >> 16) & 0xFFu) && ((t >> 8) & 0xFFu) && (t & 0xFFu));")
    if name == "TAS":
        return Instr(["const uint32 a = %s;" % Rn, "SN_RD8N(v, a)", "c.SR.T = v == 0;",
                      "SN_WR8N(a, (uint8)(v | 0x80u))"], mem=True)
    if name == "TST_R":
        return reg("c.SR.T = (%s & %s) == 0;" % (Rn, Rm))
    if name == "TST_I":
        return reg("c.SR.T = (R[0] & 0x%02Xu) == 0;" % d8)
    if name == "TST_M":
        return Instr(["SN_RD8N(v, c.GBR + R[0])", "c.SR.T = (v & 0x%02Xu) == 0;" % d8], mem=True)

    # --- multiply and accumulate: two operands from memory, the product added to MAC (saturating if SR.S) ---
    if name == "MACW":
        return Instr(["SN_RD16N(v2, %s)" % Rn, "%s += 2u;" % Rn,
                      "SN_RD16N(v1, %s)" % Rm, "%s += 2u;" % Rm,
                      "const sint32 mul = (sint32)(sint16)v1 * (sint32)(sint16)v2;",
                      "if (c.SR.S) {",
                      "const sint64 result = (sint64)(sint32)c.MAC.L + mul;",
                      "const sint64 clamped = result < -0x80000000LL ? -0x80000000LL : result > 0x7FFFFFFFLL ? 0x7FFFFFFFLL : result;",
                      "c.MAC.L = (uint32)clamped;",
                      "if (result != clamped) { c.MAC.H |= 1u; }",
                      "} else { c.MAC.u64 += (uint64)(sint64)mul; }"], mem=True)
    if name == "MACL":
        return Instr(["SN_RD32N(v2, %s)" % Rn, "%s += 4u;" % Rn,
                      "SN_RD32N(v1, %s)" % Rm, "%s += 4u;" % Rm,
                      "const sint64 op1 = (sint64)(sint32)v1; const sint64 op2 = (sint64)(sint32)v2;",
                      "uint64 result = (uint64)(op1 * op2) + c.MAC.u64;",
                      # (48-bit saturation; the bounds are compared unsigned, as the interpreter does)
                      "if (c.SR.S && result > 0x00007FFFFFFFFFFFull && result < 0xFFFF800000000000ull) {",
                      "result = (sint32)(op1 ^ op2) < 0 ? 0xFFFF800000000000ull : 0x00007FFFFFFFFFFFull; }",
                      "c.MAC.u64 = result;"], mem=True)

    # --- anything not translated here: the interpreter's handler does it (nothing, at present) ---
    if fallback:
        return Instr(["SN_HANDLER(%s)" % fallback], mem=True)
    return None


def branch_target(name, op, pc_expr):
    """For a branch: (C statements run at the branch itself, C expression of the target)."""
    n = (op >> 8) & 15
    here = pc_expr
    if name in ("BT", "BF", "BTS", "BFS"):
        return [], "%s + %du" % (here, (4 + s8(op & 0xFF) * 2) & 0xFFFFFFFF)
    if name == "BRA":
        return [], "%s + %du" % (here, (4 + s12(op & 0xFFF) * 2) & 0xFFFFFFFF)
    if name == "BSR":
        return ["c.PR = %s + 4u;" % here], "%s + %du" % (here, (4 + s12(op & 0xFFF) * 2) & 0xFFFFFFFF)
    if name == "BRAF":
        return [], "%s + R[%d] + 4u" % (here, n)
    if name == "BSRF":
        return ["c.PR = %s + 4u;" % here], "%s + R[%d] + 4u" % (here, n)
    if name == "JMP":
        return [], "R[%d]" % n
    if name == "JSR":
        return ["c.PR = %s + 4u;" % here], "R[%d]" % n
    if name == "RTS":
        return [], "c.PR"
    if name == "RTE":
        # (the target is read first, then SR; both from the stack)
        return ["SN_RD32N(rte_pc, R[15])", "SN_RD32N(rte_sr, R[15] + 4u)", "c.SR.u32 = rte_sr & 0x3F3u;",
                "R[15] += 8u;"], "rte_pc"
    raise KeyError(name)


def _reads_writes(name, op):
    """(registers read, registers written) for the instructions a waiting loop is typically made of, or None
    for anything else. Registers are numbers; memory reads don't count (they are what the loop waits on)."""
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    if name in ("MOVB_L", "MOVW_L", "MOVL_L"):
        return {m}, {n}
    if name in ("MOVB_L0", "MOVW_L0", "MOVL_L0"):
        return {m, 0}, {n}
    if name in ("MOVB_L4", "MOVW_L4"):
        return {m}, {0}
    if name == "MOVL_L4":
        return {m}, {n}
    if name in ("MOVB_LG", "MOVW_LG", "MOVL_LG"):
        return set(), {0}
    if name in ("MOV_I", "MOVW_I", "MOVL_I"):
        return set(), {n}
    if name == "MOVA":
        return set(), {0}
    if name in ("MOV_R", "EXTUB", "EXTUW", "EXTSB", "EXTSW", "SWAPB", "SWAPW", "NOT", "NEG"):
        return {m}, {n}
    if name in ("TST_R", "CMP_EQ_R", "CMP_GE", "CMP_GT", "CMP_HI", "CMP_HS", "CMP_STR"):
        return {n, m}, set()
    if name in ("CMP_PL", "CMP_PZ"):
        return {n}, set()
    if name in ("TST_I", "CMP_EQ_I"):
        return {0}, set()
    if name in ("AND_I", "OR_I", "XOR_I"):
        return {0}, {0}
    if name in ("AND_R", "OR_R", "XOR_R", "ADD", "SUB"):
        return {n, m}, {n}
    if name == "ADD_I":
        return {n}, {n}
    if name in ("NOP", "CLRT", "SETT"):
        return set(), set()
    return None


def only_waits(names, ops):
    """True if running these instructions twice leaves the same registers as running them once, and they write
    no memory: a loop made of them (plus the branch back) only waits for memory to change."""
    written = set()
    for name, op in zip(names, ops):
        rw = _reads_writes(name, op)
        if rw is None:
            return False
        written |= rw[1]
    # a register read before the body sets it, and that the body changes, carries a value from one pass to the
    # next (a counter, say): then the passes are not all alike
    seen = set()
    for name, op in zip(names, ops):
        reads, writes = _reads_writes(name, op)
        if (reads - seen) & written:
            return False
        seen |= writes
    return bool(names)
