#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
"""The calls a run made, as a tree: what a routine calls, how often, and what is a leaf (the place to start
rewriting from, docs/READABLE.md).

  SATRECOMP_NO_BLOCKS=1 SATRECOMP_CALL_GRAPH=calls.txt build/<id>/<id> ... --headless --frames N
  callgraph.py calls.txt                     every routine called: calls, callers, callees
  callgraph.py calls.txt ROOT [DEPTH]        the tree below a routine (hex address)
  callgraph.py calls.txt @SITE [DEPTH]       the trees below every routine called from one call site
                                             (a caller through pointers, such as a task list's)

A routine is known by the address called; the caller of a call site is the nearest routine start below it.
"""
import bisect
import collections
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    calls = []
    for line in open(sys.argv[1]):
        cpu, site, target, count = line.split()
        if cpu == "M":
            calls.append((int(site, 16) & 0x07FFFFFF, int(target, 16) & 0x07FFFFFF, int(count)))
    starts = sorted({t for _, t, _ in calls})
    total = collections.Counter()
    callees = collections.defaultdict(collections.Counter)
    callers = collections.defaultdict(set)
    by_site = collections.defaultdict(collections.Counter)
    for site, target, count in calls:
        total[target] += count
        by_site[site][target] += count
        i = bisect.bisect_right(starts, site) - 1
        owner = starts[i] if i >= 0 and site - starts[i] < 0x2000 else None
        if owner is not None:
            callees[owner][target] += count
            callers[target].add(owner)

    def tree(root, depth, indent, count, path):
        kids = callees.get(root, {})
        print("%s%08X  x%d%s" % ("  " * indent, root, count, "" if kids else "  (leaf)"))
        if indent >= depth or root in path:
            return
        for kid, n in sorted(kids.items(), key=lambda kv: -kv[1]):
            tree(kid, depth, indent + 1, n, path | {root})

    if len(sys.argv) == 2:
        for t in starts:
            print("%08X  x%-8d callers %-3d callees %d" % (t, total[t], len(callers[t]), len(callees.get(t, {}))))
        return
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    if sys.argv[2].startswith("@"):
        for target, n in sorted(by_site[int(sys.argv[2][1:], 16)].items(), key=lambda kv: -kv[1]):
            tree(target, depth, 0, n, frozenset())
    else:
        tree(int(sys.argv[2], 16), depth, 0, total[int(sys.argv[2], 16)], frozenset())


if __name__ == "__main__":
    main()
