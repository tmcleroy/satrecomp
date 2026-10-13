#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""For a routine about to be rewritten by hand (runtime/lift.h, docs/READABLE.md): who calls it, and which
of the registers (r0-r14 and T) its callers read after it returns.

  liveout.py DUMP ADDRESS[@RETURN,...]...     DUMP: a high work RAM dump with the code in it (--dump-ram)

  liveout.py DUMP ">ADDRESS"                  what the code from there on reads (for calling the rest of a routine)

RETURN: where a call made through a pointer comes back to (SATRECOMP_LOG_PC=address prints PR), for
routines no code names, such as those on the game's task list.

Call sites are looked for in the block database (work/<game>/blocks.txt, GAME from the environment). After
each call the caller's code is followed (both ways at conditional branches) until every such register has
been written, or read first: that one is live. A call the caller makes next is followed into the routine
called (three deep); one through a pointer reads r4-r7 as far as this tool knows; a return passes r0 up.
Anything it cannot follow is reported as unknown.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp
import sh2emit

BASE = 0x06000000
T = 16  # the T flag, tracked like a register
SCRATCH = set(range(15)) | {T}  # (r8-r14 too: hand-written code does not always put them back)
T_READERS = {"MOVT", "ADDC", "SUBC", "NEGC", "ROTCL", "ROTCR"}
T_WRITERS = {"ADDC", "ADDV", "SUBC", "SUBV", "NEGC", "ROTCL", "ROTCR", "ROTL", "ROTR", "SHAL", "SHAR", "SHLL", "SHLR", "DT",
             "CLRT", "SETT", "MOVT_", "DIV0S", "DIV0U", "DIV1", "CMP_EQ_I", "CMP_EQ_R", "CMP_GE", "CMP_GT", "CMP_HI", "CMP_HS",
             "CMP_PL", "CMP_PZ", "CMP_STR", "TAS", "TST_R", "TST_I", "TST_M"}


def name_reg(r):
    return "T" if r == T else "r%d" % r


def reads_writes(name, op):
    rw = registers(name, op)
    if rw is None:
        return None
    return rw[0] | ({T} if name in T_READERS else set()), rw[1] | ({T} if name in T_WRITERS else set())


def registers(name, op):
    n, m = (op >> 8) & 15, (op >> 4) & 15
    if name in ("MOVB_L", "MOVW_L", "MOVL_L", "MOVL_L4", "MOV_R", "EXTUB", "EXTUW", "EXTSB", "EXTSW", "SWAPB", "SWAPW",
                "NOT", "NEG", "NEGC"):
        return {m}, {n}
    if name in ("MOVB_L0", "MOVW_L0", "MOVL_L0"):
        return {m, 0}, {n}
    if name in ("MOVB_L4", "MOVW_L4"):
        return {m}, {0}
    if name in ("MOVB_LG", "MOVW_LG", "MOVL_LG", "MOVA"):
        return set(), {0}
    if name in ("MOVB_P", "MOVW_P", "MOVL_P"):
        return {m}, {n, m}
    if name in ("MOVB_S", "MOVW_S", "MOVL_S", "MOVL_S4"):
        return {n, m}, set()
    if name in ("MOVB_S0", "MOVW_S0", "MOVL_S0"):
        return {n, m, 0}, set()
    if name in ("MOVB_S4", "MOVW_S4"):
        return {m, 0}, set()
    if name in ("MOVB_SG", "MOVW_SG", "MOVL_SG", "TST_I", "CMP_EQ_I", "AND_M", "OR_M", "XOR_M", "TST_M"):
        return {0}, set()
    if name in ("MOVB_M", "MOVW_M", "MOVL_M"):
        return {n, m}, {n}
    if name in ("MOV_I", "MOVW_I", "MOVL_I", "MOVT") or name.startswith(("STC_", "STS_")) and name.endswith("_R"):
        return set(), {n}
    if name in ("ADD", "ADDC", "ADDV", "SUB", "SUBC", "SUBV", "AND_R", "OR_R", "XOR_R", "XTRCT", "DIV1"):
        return {n, m}, {n}
    if name in ("AND_I", "OR_I", "XOR_I"):
        return {0}, {0}
    if name in ("TST_R", "CMP_EQ_R", "CMP_GE", "CMP_GT", "CMP_HI", "CMP_HS", "CMP_STR", "MUL", "MULS", "MULU", "DMULS",
                "DMULU", "DIV0S"):
        return {n, m}, set()
    if name in ("CMP_PL", "CMP_PZ", "TAS") or name.startswith(("LDC_", "LDS_")) and name.endswith("_R"):
        return {n}, set()
    if name in ("ADD_I", "DT", "ROTCL", "ROTCR", "ROTL", "ROTR", "SHAL", "SHAR", "SHLL", "SHLL2", "SHLL8", "SHLL16", "SHLR",
                "SHLR2", "SHLR8", "SHLR16") or name.endswith("_M") and name.startswith(("LDC_", "LDS_", "STC_", "STS_")):
        return {n}, {n}
    if name in ("MACW", "MACL"):
        return {n, m}, {n, m}
    if name in ("NOP", "CLRT", "SETT", "CLRMAC", "DIV0U"):
        return set(), set()
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    game = os.environ.get("GAME", "fighting_vipers")
    toolkit = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # The game's repository: where this is run from, or else the one the toolkit is in
    root = os.getcwd() if os.path.isdir("games") else os.path.dirname(toolkit)
    names, decode, _ = recomp.own_tables(os.path.join(toolkit, "machine", "sh2_ops.h"))
    ram = open(sys.argv[1], "rb").read()

    def word(a):
        return struct.unpack(">H", ram[a - BASE:a - BASE + 2])[0]

    def long(a):
        return struct.unpack(">I", ram[a - BASE:a - BASE + 4])[0]

    def name_at(a):
        return names[decode[word(a)]]

    def follow(pc, unresolved, live, unknown, depth, seen, returned, calls=0):
        """Scans from pc: registers read before written go to `live`; those neither read nor written when the
        code returns go to `returned`."""
        literal = {}
        while unresolved:
            if (pc, frozenset(unresolved)) in seen:
                return  # (been here with these: a loop, followed already)
            if not (BASE <= pc < BASE + len(ram) - 2) or depth > 60:
                unknown |= unresolved
                return
            seen.add((pc, frozenset(unresolved)))
            op, name = word(pc), name_at(pc)

            def step(at):
                at_op, at_name = word(at), name_at(at)
                rw = reads_writes(at_name, at_op)
                if rw is None:
                    unknown.update(unresolved)
                    return False
                live.update(rw[0] & unresolved)
                unresolved.difference_update(rw[0] | rw[1])
                for r in rw[1]:
                    literal.pop(r, None)
                if at_name == "MOVL_I":
                    literal[(at_op >> 8) & 15] = long((at & ~3) + 4 + (at_op & 0xFF) * 4)
                return True

            if name in ("BT", "BF", "BTS", "BFS"):
                live.update({T} & unresolved)
                unresolved.discard(T)
                if name in ("BTS", "BFS") and not step(pc + 2):
                    return
                target = pc + 4 + sh2emit.s8(op & 0xFF) * 2
                follow(target, set(unresolved), live, unknown, depth + 1, seen, returned, calls)
                pc += 4 if name in ("BTS", "BFS") else 2
            elif name == "BRA":
                if not step(pc + 2):
                    return
                pc = pc + 4 + sh2emit.s12(op & 0xFFF) * 2
            elif name in ("JSR", "BSR", "BSRF"):
                n = (op >> 8) & 15
                target = pc + 4 + sh2emit.s12(op & 0xFFF) * 2 if name == "BSR" else literal.get(n) if name == "JSR" else None
                if name != "BSR":
                    live.update({n} & unresolved)
                    unresolved.discard(n)
                if not step(pc + 2):
                    return
                if target is None or calls >= 3:
                    # (a call this tool cannot follow: its arguments are read, for all it knows)
                    live.update({4, 5, 6, 7} & unresolved)
                    unresolved.clear()
                    return
                # The routine called: what it reads is read; what it neither reads nor writes on some way
                # through it is still open after the call
                after = set()
                follow(target, set(unresolved), live, unknown, depth + 1, set(), after, calls + 1)
                unresolved.intersection_update(after)
                literal.clear()
                pc += 4
            elif name == "RTS":
                if not step(pc + 2):
                    return
                returned.update(unresolved)
                return
            elif name in ("JMP", "BRAF", "RTE", "TRAPA", "SLEEP", "Illegal"):
                unknown.update(unresolved)
                return
            else:
                if not step(pc):
                    return
                pc += 2

    blocks = []
    for line in open(os.path.join(root, "work", game, "blocks.txt")):
        if line.startswith("#") or not line.strip():
            continue
        address, code = line.split()
        pc, code = int(address, 16), bytes.fromhex(code)
        if BASE <= pc < BASE + len(ram) and ram[pc - BASE:pc - BASE + len(code)] == code:
            blocks.append((pc, code))

    for argument in sys.argv[2:]:
        if argument.startswith(">"):
            # What the code from here on reads before writing it: what a rewrite of the routine's first part
            # must have in the registers when it calls the rest
            at = int(argument[1:], 16)
            live, unknown, returned = set(), set(), set()
            follow(at, set(SCRATCH), live, unknown, 0, set(), returned)
            print("%08X on: reads %s%s; still open at the return: %s" % (
                at, " ".join(name_reg(r) for r in sorted(live)) or "nothing",
                "; unknown: " + " ".join(name_reg(r) for r in sorted(unknown - live)) if unknown - live else "",
                " ".join(name_reg(r) for r in sorted(returned - live)) or "nothing"))
            continue
        target, _, returns = argument.partition("@")
        target = int(target, 16)
        sites = {(int(a, 16) - 4, "through a pointer") for a in returns.split(",") if a}
        for pc, code in blocks:
            literal = {}
            for i in range(0, len(code), 2):
                here = pc + i
                op = (code[i] << 8) | code[i + 1]
                name = names[decode[op]]
                n = (op >> 8) & 15
                if name == "MOVL_I":
                    literal[n] = long((here & ~3) + 4 + (op & 0xFF) * 4)
                elif name in ("BSR", "BRA") and here + 4 + sh2emit.s12(op & 0xFFF) * 2 == target:
                    sites.add((here, name))
                elif name in ("JSR", "JMP") and literal.get(n) == target:
                    sites.add((here, name))
                else:
                    rw = reads_writes(name, op)
                    for r in (rw[1] if rw else range(16)):
                        literal.pop(r, None)
        print("%08X: %d call sites" % (target, len(sites)))
        all_live, all_unknown = set(), set()
        for site, kind in sorted(sites):
            live, unknown = set(), set()
            if kind in ("BRA", "JMP"):
                print("  %08X %s (a jump: whoever called that routine gets what this one leaves)" % (site, kind.lower()))
                unknown = set(SCRATCH)
            else:
                returned = set()
                follow(site + 4, set(SCRATCH), live, unknown, 0, set(), returned)
                live |= returned & {0}  # (passed up as the caller's own result, perhaps)
                unknown |= returned & set(range(8, 15))  # (passed up to a caller that may count on them)
                print("  %08X%s: reads %s%s" % (site, " (" + kind + ")" if " " in kind else "", " ".join(name_reg(r) for r in sorted(live)) or "none",
                                             "; unknown: " + " ".join(name_reg(r) for r in sorted(unknown)) if unknown else ""))
            all_live |= live
            all_unknown |= unknown - live
        free = SCRATCH - all_live - all_unknown
        print("  callers read: %s%s" % (" ".join(name_reg(r) for r in sorted(all_live)) or "nothing", "; unknown: " + " ".join(
            name_reg(r) for r in sorted(all_unknown)) if all_unknown else ""))
        print("  may be left as the rewrite likes: %s   (Lift::free = 0x%05X)" % (
            " ".join(name_reg(r) for r in sorted(free)) or "none", sum(1 << r for r in free)))


if __name__ == "__main__":
    main()
