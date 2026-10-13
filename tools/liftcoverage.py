#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""Puts together what several checked runs reached of the game's routines (SATRECOMP_LIFT_COVERAGE files,
docs/READABLE.md) and lists what none of them did.

  liftcoverage.py FILE...
"""
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    reached, address = {}, {}
    for path in sys.argv[1:]:
        for line in open(path):
            name, at, bits = line.rstrip("\n").split("\t")
            address[name] = int(at, 16)
            old = reached.get(name, "0" * len(bits))
            reached[name] = "".join("1" if a == "1" or b == "1" else "0" for a, b in zip(old.ljust(len(bits), "0"), bits))
    for name in sorted(reached, key=lambda n: address[n]):
        bits = reached[name]
        ranges, i = [], 0
        while i < len(bits):
            if bits[i] == "0":
                j = i
                while j + 1 < len(bits) and bits[j + 1] == "0":
                    j += 1
                ranges.append("%08X-%08X" % (address[name] + i * 2, address[name] + j * 2))
                i = j
            i += 1
        print("%08X %-20s %4d of %4d words reached%s" % (address[name], name, bits.count("1"), len(bits),
                                                          "; not reached: " + " ".join(ranges) if ranges else ""))


if __name__ == "__main__":
    main()
