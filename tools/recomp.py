#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""satrecomp code generator: turns a block database into C++.

Input:  a block database (address + code bytes per line, written by the port with --record-blocks)
        the SH-2 decode table (written by the port with --dump-decode)
        Ymir's SH-2 sources (for the opcode names and the handler each opcode maps to)
Output: <out>/blocks_NNN.cpp   one function per block, each instruction a call to its handler with a constant opcode
        <out>/blocks_index.cpp the table of all generated files

Nothing here is specific to a game. Generated files are derived from the game (and BIOS) code: never commit them.
"""

import argparse
import os
import re
import struct
import sys

import sh2emit
from sh2dis import disasm

UNCOND_DELAYED = {"BRA", "BRAF", "BSR", "BSRF", "JMP", "JSR", "RTS", "RTE"}
COND_DELAYED = {"BTS", "BFS"}
ENDS_BLOCK = {"Illegal", "SLEEP", "TRAPA"}
MAX_BLOCK_INSTRS = 48


def parse_opcode_names(decode_hpp):
    """OpcodeType enumerators in order (their index is the value stored in the decode table)."""
    text = open(decode_hpp).read()
    m = re.search(r"enum class OpcodeType\s*:\s*\w+\s*\{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit("cannot find enum class OpcodeType in " + decode_hpp)
    names = []
    for line in m.group(1).split("\n"):
        line = line.split("//")[0].strip()
        if not line:
            continue
        for part in line.split(","):
            part = part.strip()
            if part:
                if "=" in part:
                    sys.exit("OpcodeType with explicit values is not supported: " + part)
                names.append(part)
    return names


def own_tables(ops_h):
    """The decode table from satrecomp's own instruction list (machine/sh2_ops.h): the names of the kinds of
    instruction, the kind of every opcode (as itself and as a delay slot instruction) and its nominal cycles.
    The same as the reference machine's table."""
    text = open(ops_h).read()
    m = re.search(r"enum class Op\s*:\s*\w+\s*\{(.*?)\};", text, re.S)
    names = [n.strip() for n in m.group(1).replace("\n", " ").split(",") if n.strip()]
    patterns = [(names.index(n), int(mask, 16), int(match, 16))
                for n, mask, match in re.findall(r"\{Op::(\w+), 0x([0-9A-F]+), 0x([0-9A-F]+)\}", text)]
    illegal = names.index("Illegal")
    no_slot = set(names.index(n) for n in ("BRA", "BRAF", "BSR", "BSRF", "JMP", "JSR", "RTS", "RTE", "BT", "BF", "BTS",
                                           "BFS", "TRAPA", "Illegal"))
    slot_names = ["Delay_" + n for n in names] + ["IllegalSlot"]
    plain, delay, nominal = [], [], bytearray()
    two = set(names.index(n) for n in ("BRA", "BRAF", "BSR", "BSRF", "JMP", "JSR", "RTS", "BT", "BF", "BTS", "BFS", "MUL",
                                       "DMULS", "DMULU"))
    three = set(names.index(n) for n in ("MACW", "MACL", "AND_M", "OR_M", "XOR_M", "TST_M", "LDC_GBR_M", "LDC_SR_M",
                                         "LDC_VBR_M", "STC_GBR_M", "STC_SR_M", "STC_VBR_M"))
    four = set(names.index(n) for n in ("TAS", "RTE"))
    for op in range(0x10000):
        kind = illegal
        for index, mask, match in patterns:
            if op & mask == match:
                kind = index
                break
        plain.append(kind)
        delay.append(len(names) + len(slot_names) - 1 if kind in no_slot else len(names) + kind)
        nominal.append(2 if kind in two else 3 if kind in three else 4 if kind in four else 1)
    return names + slot_names, plain + delay, bytes(nominal)


def parse_handlers(sh2_cpp):
    """OpcodeType name -> C++ call text with the interpreter's template arguments made constant.

    Taken from the interpreter's dispatch switch, e.g.
        case OpcodeType::MOVL_I: return MOVLI<debug, emulateCache, false>(instr);
    gives  MOVL_I -> 'MOVLI<false, false, false>(%s)'
    """
    text = open(sh2_cpp).read()
    handlers = {}
    for m in re.finditer(r"case OpcodeType::(\w+):\s*return\s+(\w+)(<[^>]*>)?\((instr)?\);", text):
        name, fn, targs, arg = m.groups()
        targs = (targs or "").replace("debug", "false").replace("emulateCache", "false")
        handlers[name] = fn + targs + "(" + ("%s" if arg else "") + ")"
    return handlers


IMPURE_MARKERS = ("MemRead", "MemWrite", "AccessCycles", "IsBusWait", "SetupDelaySlot", "EnterException", "SR.u32 =",
                  "RecalcInterrupts", "m_sleep", "ExecuteDiv", "RefillPipeline")
# Loads from the literal pool next to the code: plain memory, never a bus wait or a hardware side effect
LITERAL_LOADS = {"MOVWI", "MOVLI"}


def parse_handler_traits(sh2_cpp):
    """Handler function name -> (pure, clears_allow).

    pure: only touches CPU registers and always continues with the next instruction, so nothing needs checking
          after it.
    clears_allow: holds off interrupts for one instruction (ldc/lds/stc/sts), so the instruction after it is one
          after which an interrupt can be due.
    """
    text = open(sh2_cpp).read()
    traits = {}
    for m in re.finditer(r"^FORCE_INLINE(?:_EX)? uint64 SH2::(\w+)\([^)]*\) \{\n(.*?)^\}", text, re.S | re.M):
        name, body = m.groups()
        pure = name in LITERAL_LOADS or not any(k in body for k in IMPURE_MARKERS)
        traits[name] = (pure, "allow = false" in body)
    return traits


def load_blocks(path, regions):
    blocks = {}
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        addr, code = line.split()
        pc = int(addr, 16)
        if not any(lo <= pc < hi for lo, hi in regions):
            continue
        blocks[(pc, code)] = bytes.fromhex(code)
    return [(pc, code) for (pc, _), code in sorted(blocks.items())]


class Generator:
    def __init__(self, names, handlers, traits, decode, nominal):
        self.names = names
        self.handlers = handlers
        self.traits = traits
        self.plain = decode[:0x10000]
        self.delay = decode[0x10000:]
        self.nominal = nominal  # nominal cycles per opcode (the native code path's time)

    def successors(self, pc, code):
        """Addresses a block can go on to by a branch with a fixed target, by running off its end, or when the
        call it ends with comes back (not returns or computed jumps): the transfers that can stay inside one
        native function."""
        ops = struct.unpack(">%dH" % (len(code) // 2), code)
        names = [self.names[self.plain[op]] for op in ops]
        out = []
        i = 0
        n = len(ops)
        while i < n:
            name = names[i]
            here = pc + i * 2
            if name in ("BT", "BF"):
                out.append(here + 4 + sh2emit.s8(ops[i] & 0xFF) * 2)
            elif name in ("BTS", "BFS"):
                out.append(here + 4 + sh2emit.s8(ops[i] & 0xFF) * 2)
            elif name in UNCOND_DELAYED:
                if name == "BRA":
                    out.append(here + 4 + sh2emit.s12(ops[i] & 0xFFF) * 2)
                elif name in ("JSR", "BSR", "BSRF") and i + 2 == n:
                    out.append(pc + n * 2)  # where the call comes back to
                return out
            i += 1
        out.append(pc + n * 2)
        return out

    def native(self, pc, code, members, base, label, index):
        """Returns the lines of a block in native code (tools/sh2emit.py), or None.
        members: address -> label of the blocks in the same function (this one included, unless it may not be
        branched to within it); a branch to one of them is a goto. base: address of the function's first block.
        label: this block's label. index: address -> number of the block in the function (as in members)."""
        ops = struct.unpack(">%dH" % (len(code) // 2), code)
        n = len(ops)
        lines = []
        cycles = [0]
        for op in ops:
            cycles.append(cycles[-1] + self.nominal[op])
        names = [self.names[self.plain[op]] for op in ops]

        def wb_stmt(wb):
            return "" if wb is None else "c.m_wbReg = 0x%02X; " % wb

        def leave(index, cyc, wb):
            return "{ %sSN_LEAVE(%d, %d) }" % (wb_stmt(wb), index * 2, cyc)

        def go(dest, expr, cyc, idle=False):
            """Statement for control going to address `dest` (C expression `expr`), cyc cycles into the block."""
            if dest in members:
                if idle:
                    return "SN_GOTO_IDLE(%s, %d, %d, %d)" % (label, dest - base, cyc, index[dest])
                return "SN_GOTO(%s, %d, %d, %d, %d)" % (members[dest], dest - base, cyc, 1 if dest <= pc else 0, index[dest])
            return "SN_JUMP(%s, %d)" % (expr, cyc)

        def call(expr, cyc):
            """Statement for a call to `expr`, the instruction after it being the one after this block."""
            end = pc + n * 2
            if end in members:
                return "SN_CALL(%s, %d, %d, %d, goto %s;)" % (expr, cyc, end - base, index[end], members[end])
            return "SN_CALL(%s, %d, %d, -1, )" % (expr, cyc, end - base)

        def instr(i, wb_before, pcx, in_slot):
            """One instruction at index i; returns (Instr, statements)."""
            on_wait = leave(i, cycles[i + 1], wb_before)
            fallback = self.call(self.delay if in_slot else self.plain, ops[i]) if self.handlers else None
            ins = sh2emit.emit(names[i], ops[i], pcx, on_wait, in_slot, fallback)
            if ins is None:
                return None, None
            body = "{ SN_I(%d, %d) %s }" % (i * 2, cycles[i], " ".join(ins.lines)) if ins.lines else ";"
            return ins, body

        def fixed_target(i):
            if names[i] in ("BT", "BF", "BTS", "BFS"):
                return pc + i * 2 + 4 + sh2emit.s8(ops[i] & 0xFF) * 2
            if names[i] in ("BRA", "BSR"):
                return pc + i * 2 + 4 + sh2emit.s12(ops[i] & 0xFFF) * 2
            return None

        # A countdown at the start of the block (dt Rn; bf to itself), e.g. a delay loop. Entered from elsewhere
        # the count is skipped ahead first; the loop itself branches to the label after that.
        entry_label = label
        if n >= 2 and names[0] == "DT" and names[1] == "BF" and fixed_target(1) == pc and pc in members:
            entry_label = label + "_in"
            lines.append("%s: SN_COUNTDOWN(%d, %d)" % (entry_label, (ops[0] >> 8) & 15, cycles[2]))
        lines.append("SN_BLOCK(%s, %d)" % (label, pc - base))

        i = 0
        wb = None        # m_wbReg as the previous instruction left it (None: whatever it was before the block)
        allow_off = False  # the previous instruction held off interrupts
        first_branch = True
        while i < n:
            name = names[i]
            if name in ENDS_BLOCK:
                return None
            text = "// %08X  %s" % (pc + i * 2, disasm(ops[i], pc + i * 2))
            if allow_off:
                lines.append("c.m_intrFlags.allow = true;")
                allow_off = False
            if name in ("BT", "BF"):
                _, expr = sh2emit.branch_target(name, ops[i], "pc + %du" % (i * 2))
                cond = "c.SR.T" if name == "BT" else "!c.SR.T"
                dest = fixed_target(i)
                # a loop back to the start of the block that only waits for memory to change
                idle = dest == pc and first_branch and sh2emit.only_waits(names[:i], ops[:i])
                # (the first instruction's bus-wait exit keeps m_wbReg as it is, so it is set here)
                lines.append("if (%s) { c.m_wbReg = 0xFF; %s } %s" % (cond, go(dest, expr, cycles[i + 1], idle), text))
                wb = sh2emit.WB_NONE
                first_branch = False
                i += 1
                continue
            if name in UNCOND_DELAYED or name in COND_DELAYED:
                first_branch = False
                if i + 1 >= n:
                    return None
                slot_text = "// %08X  %s" % (pc + i * 2 + 2, disasm(ops[i + 1], pc + i * 2 + 2))
                pre, expr = sh2emit.branch_target(name, ops[i], "pc + %du" % (i * 2))
                slot, slot_body = instr(i + 1, sh2emit.WB_NONE, "(dest - 2u)", True)
                if slot is None:
                    return None
                dest = fixed_target(i) if name != "BSR" else None
                taken = ["{ SN_I(%d, %d) %s" % (i * 2, cycles[i], " ".join(pre)),
                         "  const uint32 dest = %s;" % expr,
                         "  SN_SLOT_BEGIN(dest) %s" % text,
                         "  %s %s" % (slot_body, slot_text)]
                if slot.clears_allow:
                    taken.append("  c.m_intrFlags.allow = false;")
                if name in ("JSR", "BSR", "BSRF") and i + 2 == n:
                    leave_by = call("dest", cycles[i + 2])
                else:
                    leave_by = go(dest, "dest", cycles[i + 2])
                taken.append("  SN_SLOT_END() c.m_wbReg = 0x%02X; %s }" % (slot.wb, leave_by))
                if name in UNCOND_DELAYED:
                    if i + 2 != n:
                        return None
                    lines.extend(taken)
                    return lines, entry_label
                cond = "c.SR.T" if name == "BTS" else "!c.SR.T"
                lines.append("if (%s)" % cond)
                lines.extend(taken)
                wb = sh2emit.WB_NONE
                i += 1  # not taken: the next instruction is an ordinary one
                continue
            ins, body = instr(i, wb, "pc + %du" % (i * 2), False)
            if ins is None:
                return None
            lines.append("%s %s" % (body, text))
            wb = ins.wb
            if ins.clears_allow:
                lines.append("c.m_intrFlags.allow = false;")
                allow_off = True
            if ins.mem or ins.sr:
                lines.append("if (c.m_intrFlags.pending) [[unlikely]] %s" % leave(i + 1, cycles[i + 1], wb))
            i += 1
        # running off the end of the block
        end = pc + n * 2
        if end in members:
            lines.append("{ %s%s }" % (wb_stmt(wb), go(end, None, cycles[n])))
        else:
            lines.append(leave(n, cycles[n], wb))
        return lines, entry_label

    def function(self, blocks, stops):
        """Native function body for a list of (pc, code) blocks, the first being its entry; None if any of
        them cannot be translated. Blocks at sync points are never branched to inside a function."""
        base = blocks[0][0]
        labels = {pc: "B%d" % k for k, (pc, _) in enumerate(blocks) if pc not in stops}
        index = {pc: k for k, (pc, _) in enumerate(blocks)}
        bodies = []
        # (a block that starts with a countdown is entered through its second label)
        for _ in range(2):
            bodies = []
            entries = {}
            for k, (pc, code) in enumerate(blocks):
                r = self.native(pc, code, labels, base, "B%d" % k, index)
                if r is None:
                    return None
                bodies.append(r[0])
                entries[pc] = r[1]
            changed = False
            for pc in labels:
                if entries[pc] != labels[pc] and not labels[pc].endswith("_in"):
                    labels[pc] = entries[pc]
                    changed = True
            if not changed:
                break
        # Started at a block other than the first (taken up again where it stopped, or after a call)
        out = []
        if len(blocks) > 1:
            out.append("static const int32_t kBlockAt[] = {%s};" % ", ".join(str(bpc - base) for bpc, _ in blocks))
            out.append("SN_BEGIN(c.PC - kBlockAt[start])")
            out.append("switch (start) {")
            for k, (bpc, _) in enumerate(blocks[1:], 1):
                if bpc in labels:
                    out.append("case %d: goto %s;" % (k, labels[bpc]))
            out.append("default: break;")
            out.append("}")
        else:
            out.append("SN_BEGIN(c.PC)")
            out.append("(void)start;")
        for body in bodies:
            out.extend(body)
        return out

    def call(self, table, opcode):
        """C++ call text of the interpreter's handler for an opcode."""
        name = self.names[table[opcode]]
        h = self.handlers.get(name)
        if h is None:
            raise KeyError(name)
        return h % ("0x%04X" % opcode) if "%s" in h else h

    def block(self, pc, code):
        """Returns the body lines for a block, or None if it doesn't follow the runtime's block rules."""
        ops = struct.unpack(">%dH" % (len(code) // 2), code)
        n = len(ops)
        if n == 0 or n > MAX_BLOCK_INSTRS:
            return None
        lines = []
        i = 0
        check_next = False  # the previous instruction held off interrupts: one can be due after this one
        while i < n:
            name = self.names[self.plain[ops[i]]]
            if name in ENDS_BLOCK:
                return None
            text = "// %08X  %s" % (pc + i * 2, disasm(ops[i], pc + i * 2))
            if name in UNCOND_DELAYED or name in COND_DELAYED:
                if i + 1 >= n:
                    return None
                slot = ops[i + 1]
                if self.names[self.delay[slot]] == "IllegalSlot" or self.names[self.plain[slot]] in ENDS_BLOCK:
                    return None
                slot_text = "// %08X  %s" % (pc + i * 2 + 2, disasm(slot, pc + i * 2 + 2))
                if name in UNCOND_DELAYED:
                    if i + 2 != n:
                        return None
                    lines.append("SR_AT(%d) SR_EXEC(%s) SR_SLOT_FOLLOWS() %s" % (i, self.call(self.plain, ops[i]), text))
                    lines.append("        SR_EXEC(%s) return; %s" % (self.call(self.delay, slot), slot_text))
                else:
                    # taken: the next instruction is the delay slot, then control leaves (or, for a branch to
                    # the instruction after the slot, goes on); not taken: it is an ordinary instruction
                    lines.append("SR_AT(%d) SR_EXEC(%s) SR_NEXT() %s" % (i, self.call(self.plain, ops[i]), text))
                    lines.append("        if (SR_BRANCH_TAKEN()) { SR_EXEC(%s) return; }" % self.call(self.delay, slot))
                    lines.append("        SR_NEXT_CHECKED(%d)" % (i + 1))
                    lines.append("SR_AT(%d) SR_EXEC(%s) SR_NEXT_CHECKED(%d) %s" % (
                        i + 1, self.call(self.plain, slot), i + 2, slot_text))
                i += 2
                # (a bt/s or bf/s slot instruction run as an ordinary one may itself have held off interrupts)
                check_next = name in COND_DELAYED
            else:
                call = self.call(self.plain, ops[i])
                pure, clears_allow = self.traits.get(call.split("<")[0].split("(")[0], (False, False))
                if pure and not check_next:
                    lines.append("SR_AT(%d) SR_EXEC(%s) SR_NEXT() %s" % (i, call, text))
                else:
                    lines.append("SR_AT(%d) SR_EXEC(%s) SR_NEXT_CHECKED(%d) %s" % (i, call, i + 1, text))
                check_next = clears_allow
                i += 1
        return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--blocks", required=True, help="block database")
    ap.add_argument("--decode", help="decode table from the reference build's --dump-decode")
    ap.add_argument("--ymir", help="Ymir source tree: also emit the cycle-exact blocks made of its interpreter's "
                                   "handlers (for the reference build)")
    ap.add_argument("--ops", help="satrecomp/machine/sh2_ops.h: the instruction list, when not using --ymir")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--per-file", type=int, default=400, help="blocks per generated file")
    ap.add_argument("--max-function-blocks", type=int, default=64, help="blocks joined into one native function")
    ap.add_argument("--stops", default="",
                    help="addresses (hex) blocks may not run across: the game's sync points (game.cfg)")
    ap.add_argument("--region", action="append", default=[],
                    help="LO-HI (hex): only generate blocks in these address ranges (default: everything)")
    args = ap.parse_args()

    exact = bool(args.ymir)
    if exact:
        core = os.path.join(args.ymir, "libs", "ymir-core")
        names = parse_opcode_names(os.path.join(core, "include", "ymir", "hw", "sh2", "sh2_decode.hpp"))
        handlers = parse_handlers(os.path.join(core, "src", "ymir", "hw", "sh2", "sh2.cpp"))
        raw = open(args.decode, "rb").read()
        if len(raw) != 6 * 0x10000:
            sys.exit("unexpected decode table size (rebuild the port and dump it again)")
        decode = struct.unpack("<%dH" % (2 * 0x10000), raw[:4 * 0x10000])
        nominal = raw[4 * 0x10000:5 * 0x10000]
        if max(decode) >= len(names):
            sys.exit("decode table doesn't match the OpcodeType enum (rebuild the port and dump it again)")
    else:
        names, decode, nominal = own_tables(args.ops)
        handlers = {}

    regions = []
    for r in args.region:
        lo, hi = r.split("-")
        regions.append((int(lo, 16), int(hi, 16)))
    if not regions:
        regions = [(0, 1 << 32)]

    traits = parse_handler_traits(os.path.join(core, "src", "ymir", "hw", "sh2", "sh2.cpp")) if exact else {}
    stops = [int(x, 16) for x in args.stops.replace(",", " ").split()]
    gen = Generator(names, handlers, traits, decode, nominal)
    blocks = load_blocks(args.blocks, regions)
    # A block recorded across a sync point becomes two (the runtime ends blocks there)
    split = {}
    for pc, code in blocks:
        cut = [a for a in stops if pc < a < pc + len(code)]
        if cut:
            at = min(cut) - pc
            split[(pc, code[:at])] = True
            split[(pc + at, code[at:])] = True
        else:
            split[(pc, code)] = True
    blocks = sorted(split)
    os.makedirs(args.out, exist_ok=True)

    # Functions: blocks joined by branches with fixed targets. Only addresses with a single version of code
    # take part (where programs are loaded over one another, each block stays on its own).
    count = {}
    for pc, code in blocks:
        count[pc] = count.get(pc, 0) + 1
    nodes = {pc: code for pc, code in blocks if count[pc] == 1 and pc not in stops}
    succ = {pc: [t for t in gen.successors(pc, code) if t in nodes] for pc, code in nodes.items()}
    has_pred = set(t for pc in succ for t in succ[pc] if t != pc)
    functions = {}
    assigned = set()
    order = [pc for pc in sorted(nodes) if pc not in has_pred] + [pc for pc in sorted(nodes) if pc in has_pred]
    for head in order:
        if head in assigned:
            continue
        members = [head]
        assigned.add(head)
        stack = [head]
        while stack and len(members) < args.max_function_blocks:
            for t in succ[stack.pop()]:
                if t not in assigned and len(members) < args.max_function_blocks:
                    assigned.add(t)
                    members.append(t)
                    stack.append(t)
        if len(members) > 1:
            functions[head] = [(head, nodes[head])] + sorted((m, nodes[m]) for m in members[1:])
    total_functions = 0

    files = []
    natives = 0
    has_native = {}
    skipped = 0
    instrs = 0
    for start in range(0, len(blocks), args.per_file):
        index = len(files)
        out = []
        out.append("// Generated by satrecomp/tools/recomp.py from game code. Do not edit, do not commit.")
        if exact:
            out.append("#ifndef SATRECOMP_OWN_MACHINE")
            out.append("#define SATRECOMP_OPS_ONLY")
            out.append("#include <ymir/hw/sh2/sh2.cpp>")
            out.append("#endif")
        out.append("")
        out.append('#include "block_native.h"')
        out.append('#include "block_ops.h"')
        out.append("")
        out.append("namespace satrecomp {")
        out.append("")
        out.append("template <>")
        out.append("struct BlockSet<%d> {" % index)
        descs = []
        func_descs = []
        for pc, code in blocks[start:start + args.per_file]:
            native = gen.function([(pc, code)], stops)
            body = gen.block(pc, code) if exact else None
            if (body if exact else native) is None:
                skipped += 1
                continue
            instrs += len(code) // 2
            ident = "b%08X_%d" % (pc, len(descs))
            if exact:
                out.append("#ifndef SATRECOMP_OWN_MACHINE")
                out.append("    static void %s(SH2 &c, uint64_t target, unsigned start) {" % ident)
                out.append("        SR_BLOCK_BEGIN()")
                for line in body:
                    out.append("        " + line)
                out.append("        SR_BLOCK_END()")
                out.append("    }")
                out.append("#endif")
            if native is not None:
                natives += 1
                out.append("    static void n%s(SH2 &c, uint64_t target, unsigned start) {" % ident[1:])
                for line in native:
                    out.append("        " + line)
                out.append("    }")
            has_native[ident] = native is not None
            # The function this block is the entry of: the blocks reached from it by branches
            members = functions.get(pc)
            if members and members[0][1] == code:
                body = gen.function(members, stops)
                if body is not None:
                    out.append("    static void f%s(SH2 &c, uint64_t target, unsigned start) {" % ident[1:])
                    for line in body:
                        out.append("        " + line)
                    out.append("    }")
                    func_descs.append((ident, members))
            descs.append((pc, code, ident))
        out.append("};")
        out.append("")
        for pc, code, ident in descs:
            out.append("static const uint8_t code_%s[] = {%s};" % (ident, ",".join("0x%02X" % b for b in code)))
        out.append("")
        for ident, members in func_descs:
            for k, (mpc, mcode) in enumerate(members[1:], 1):
                out.append("static const uint8_t fcode_%s_%d[] = {%s};" % (
                    ident, k, ",".join("0x%02X" % b for b in mcode)))
            out.append("static const FunctionBlock fblocks_%s[] = {" % ident)
            for k, (mpc, mcode) in enumerate(members):
                out.append("    {%d, %d, %s}," % (mpc - members[0][0], len(mcode),
                                                  "code_%s" % ident if k == 0 else "fcode_%s_%d" % (ident, k)))
            out.append("};")
        out.append("extern const FunctionDesc g_functions_%d[];" % index)
        out.append("const FunctionDesc g_functions_%d[] = {" % index)
        for ident, members in func_descs:
            out.append("    {0x%08X, &BlockSet<%d>::f%s, fblocks_%s, %d}," % (
                members[0][0], index, ident[1:], ident, len(members)))
        if not func_descs:
            out.append("    {0, nullptr, nullptr, 0},")
        out.append("};")
        out.append("extern const size_t g_functions_%d_count;" % index)
        out.append("const size_t g_functions_%d_count = %d;" % (index, len(func_descs)))
        total_functions += len(func_descs)
        out.append("")
        out.append("extern const BlockDesc g_blocks_%d[];" % index)
        out.append("const BlockDesc g_blocks_%d[] = {" % index)
        for pc, code, ident in descs:
            native = "&BlockSet<%d>::n%s" % (index, ident[1:]) if has_native[ident] else "nullptr"
            out.append("    {0x%08X, %d, code_%s, %s, %s}," % (
                pc, len(code) // 2, ident, "SR_FN(&BlockSet<%d>::%s)" % (index, ident) if exact else "nullptr", native))
        out.append("};")
        out.append("extern const size_t g_blocks_%d_count;" % index)
        out.append("const size_t g_blocks_%d_count = %d;" % (index, len(descs)))
        out.append("")
        out.append("} // namespace satrecomp")
        files.append(("blocks_%03d.cpp" % index, "\n".join(out) + "\n"))

    idx = []
    idx.append("// Generated by satrecomp/tools/recomp.py. Do not edit, do not commit.")
    idx.append('#include "blocks.h"')
    idx.append("")
    idx.append("namespace satrecomp {")
    idx.append("")
    for i in range(len(files)):
        idx.append("extern const BlockDesc g_blocks_%d[];" % i)
        idx.append("extern const size_t g_blocks_%d_count;" % i)
        idx.append("extern const FunctionDesc g_functions_%d[];" % i)
        idx.append("extern const size_t g_functions_%d_count;" % i)
    idx.append("")
    idx.append("const BlockTable g_blockTables[] = {")
    for i in range(len(files)):
        idx.append("    {g_blocks_%d, g_blocks_%d_count, g_functions_%d, g_functions_%d_count}," % (i, i, i, i))
    if not files:
        idx.append("    {nullptr, 0, nullptr, 0},")
    idx.append("};")
    idx.append("const size_t g_blockTableCount = %d;" % len(files))
    idx.append("")
    idx.append("} // namespace satrecomp")
    files.append(("blocks_index.cpp", "\n".join(idx) + "\n"))

    # Only touch files whose content changed, so unchanged ones aren't recompiled
    wanted = set()
    written = 0
    for name, content in files:
        path = os.path.join(args.out, name)
        wanted.add(name)
        if os.path.exists(path) and open(path).read() == content:
            continue
        with open(path, "w") as f:
            f.write(content)
        written += 1
    for name in os.listdir(args.out):
        if name.startswith("blocks_") and name.endswith(".cpp") and name not in wanted:
            os.remove(os.path.join(args.out, name))

    print("recomp: %d blocks (%d with native code), %d functions of several blocks, %d instructions, "
          "%d files (%d written), %d blocks skipped"
          % (len(blocks) - skipped, natives, total_functions, instrs, len(files) - 1, written, skipped))


if __name__ == "__main__":
    main()
