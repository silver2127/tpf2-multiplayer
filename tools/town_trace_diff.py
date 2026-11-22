#!/usr/bin/env python3
"""Name the first town development decision two peers disagree on.

    python tools/town_trace_diff.py A/tpf2_towntrace.txt B/tpf2_towntrace.txt [--all]

Input: the trace both builds write when enabled (Windows: towntrace=1 in
tpf2_slice.cfg; native Linux: TPF2MP_TOWN_TRACE=1), format in
native/src/town_trace.h. Only the time window both files cover is compared
(a joiner starts at its load, the host earlier). A trace may hold several
sessions; compare files from the same session.

TT (one per Develop call), keyed by (t, town):
  list differs            the Town node list is in another order: the stagger
                          (t % 120 == (i % 30) * 4) and the shared per-tick
                          generator reach the towns differently -- node-list
                          order, not a town decision
  i/n differ, list same   impossible unless the list content differs
  mt0 differs             an EARLIER Develop of the same tick drew differently
  mt0 same, mt1 differs   THIS town's Develop decided differently: the town
                          and tick to trace deeper
  present on one side     the town was developed on one side only (gating:
                          the Town component's +0x61 flag, capacity)
TF (per 600 iterations): each peer's node lists as (count:digest) tokens; a
token on one side only is a list whose content or order differs.
"""
import re
import sys
from collections import defaultdict

TT = re.compile(r'^TT t=(-?\d+) town=(-?\d+) i=(-?\d+) n=(-?\d+) list=([0-9a-f]{8}) mt0=([0-9a-f]{16}) mt1=([0-9a-f]{16}) e=(\d+)$')
TF = re.compile(r'^TF t=(-?\d+) e=(\d+) lists=(\d+)((?: \d+:[0-9a-f]{8})*)$')


def load(path):
    tt, tf = {}, defaultdict(list)
    dup = 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            m = TT.match(line)
            if m:
                t, town, i, n, lst, mt0, mt1, e = m.groups()
                key = (int(t), int(town))
                if key in tt:
                    dup += 1          # a second engine replaying the same tick
                    continue
                tt[key] = dict(i=int(i), n=int(n), list=lst, mt0=mt0, mt1=mt1)
                continue
            m = TF.match(line)
            if m:
                t, e, k, toks = m.groups()
                tf[int(t) // 600].append(tuple(sorted(toks.split())))
    return tt, tf, dup


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a_tt, a_tf, a_dup = load(sys.argv[1])
    b_tt, b_tf, b_dup = load(sys.argv[2])
    show_all = '--all' in sys.argv
    if not a_tt or not b_tt:
        print('no TT lines in', sys.argv[1] if not a_tt else sys.argv[2])
        return 1
    lo = max(min(t for t, _ in a_tt), min(t for t, _ in b_tt))
    hi = min(max(t for t, _ in a_tt), max(t for t, _ in b_tt))
    print(f'A: {len(a_tt)} Develop calls, B: {len(b_tt)}; common window t={lo}..{hi} '
          f'({(hi - lo) / 5:.1f} game units)')
    keys = sorted(k for k in set(a_tt) | set(b_tt) if lo <= k[0] <= hi)
    diffs = 0
    for key in keys:
        a, b = a_tt.get(key), b_tt.get(key)
        t, town = key
        if a == b:
            continue
        if a is None or b is None:
            side = 'A' if a else 'B'
            why = f'developed on {side} only'
        elif a['list'] != b['list']:
            why = f"Town node list differs (A {a['list']} i={a['i']}/{a['n']}, B {b['list']} i={b['i']}/{b['n']}): node-list order"
        elif a['mt0'] != b['mt0']:
            why = 'generator differs on entry: an earlier Develop of this tick drew differently'
        else:
            why = 'same inputs, generator differs on exit: THIS town decided differently'
        print(f'  t={t} ({t / 5:.1f} units) town={town}: {why}')
        diffs += 1
        if diffs >= 12 and not show_all:
            print('  ... (--all for every difference)')
            break
    if not diffs:
        print('  every Develop call in the common window agrees')
    for w in sorted(set(a_tf) & set(b_tf)):
        if lo // 600 <= w <= hi // 600:
            sa, sb = set(a_tf[w]), set(b_tf[w])
            if not sa & sb:
                ta, tb = set(a_tf[w][0]), set(b_tf[w][0])
                print(f'  TF window t={w * 600}: node lists differ; tokens only in A: {sorted(ta - tb)[:6]} only in B: {sorted(tb - ta)[:6]}')
                break
    return 0 if not diffs else 1


if __name__ == '__main__':
    sys.exit(main())
