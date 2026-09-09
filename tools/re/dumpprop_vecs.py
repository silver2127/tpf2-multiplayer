#!/usr/bin/env python3
"""Decode the DV<c>_<off>_ vector dumps written by the hook's DUMPPROP path.

For the UI (c=1) and Lua (c=2) construction placements, print the node
records (24 B: x y z flags type id) at r8+000, the segment records (120 B:
node0 @+08, node1 @+0c, t0 @+10, t1 @+1c, rest hex) at r8+018, the removed
segments at r8+048, and the first bytes of every other vector, so the two
proposals can be compared element by element.

    python tools/dumpprop_vecs.py [-b] [-u N] [-l N]
"""
import re, struct, sys
from dumpprop_diff import load, REAL, OVL

def dumps_for(lines, c, off):
    """All dumps of vector at r8+off for side c, in log order."""
    tag = "DV%d_%03x_" % (c, off)
    pat = re.compile(r"\[gt\] " + re.escape(tag) + r"0\.(\d+)\+([0-9a-f]{3}):([0-9a-f]+)")
    dumps, cur, curk = [], {}, None
    for ln in lines:
        m = pat.search(ln)
        if not m: continue
        k, o = int(m.group(1)), int(m.group(2), 16)
        if o == 0 and cur: dumps.append(cur); cur = {}
        cur[o] = bytes.fromhex(m.group(3))
    if cur: dumps.append(cur)
    out = []
    for d in dumps:
        b = bytearray()
        for o in sorted(d): b += d[o]
        out.append(bytes(b))
    return out

def spans(lines, c):
    pat = re.compile(r"\[slice\] DVEC c=%d r8\+([0-9a-f]{3}) span=(\d+)" % c)
    res = {}
    for ln in lines:
        m = pat.search(ln)
        if m: res.setdefault(int(m.group(1), 16), []).append(int(m.group(2)))
    return res

def nodes(b, n):
    for i in range(n):
        if (i + 1) * 24 > len(b): break
        x, y, z, fl, ty, nid = struct.unpack_from("<3f2Ii", b, i * 24)
        print(f"    node[{i}] id={nid} pos=({x:.3f},{y:.3f},{z:.3f}) flags={fl:#x} type={ty}")

def segs(b, n):
    for i in range(n):
        o = i * 120
        if o + 120 > len(b): print(f"    seg[{i}] (truncated dump)"); break
        head = struct.unpack_from("<2I", b, o)
        n0, n1 = struct.unpack_from("<2i", b, o + 8)
        t0 = struct.unpack_from("<3f", b, o + 0x10); t1 = struct.unpack_from("<3f", b, o + 0x1c)
        rest = b[o + 0x28:o + 120].hex()
        print(f"    seg[{i}] head={head[0]:#x},{head[1]:#x} {n0}->{n1} t0=({t0[0]:.2f},{t0[1]:.2f},{t0[2]:.2f}) t1=({t1[0]:.2f},{t1[1]:.2f},{t1[2]:.2f})")
        print(f"           +28: {rest[:96]}")
        print(f"           +58: {rest[96:]}")

def main():
    path = OVL if "-b" in sys.argv else REAL
    idx = {1: -1, 2: -1, 3: -1}
    a = sys.argv[1:]
    for i, s in enumerate(a):
        if s == "-u": idx[1] = int(a[i + 1])
        if s == "-l": idx[2] = int(a[i + 1])
    lines = load(path)
    for c, name in ((1, "UI"), (2, "Lua"), (3, "Merged")):
        sp = spans(lines, c)
        print(f"===== side {name} (c={c}); vectors seen (offset: spans per dump):")
        for off in sorted(sp): print(f"    +{off:03x}: {sp[off]}")
        for off, kind in ((0x000, "nodes"), (0x018, "segments"), (0x048, "removed segs"), (0x030, "removed nodes")):
            ds = dumps_for(lines, c, off)
            if not ds: continue
            d = ds[idx[c]] if -len(ds) <= idx[c] < len(ds) else ds[-1]
            print(f"  -- r8+{off:03x} {kind}: {len(d)} bytes in dump")
            if kind == "nodes" or kind == "removed nodes": nodes(d, len(d) // 24)
            else: segs(d, max(1, len(d) // 120))
        for off in sorted(sp):
            if off in (0x000, 0x018, 0x048, 0x030): continue
            ds = dumps_for(lines, c, off)
            if not ds: continue
            d = ds[idx[c]] if -len(ds) <= idx[c] < len(ds) else ds[-1]
            ints = struct.unpack_from("<%di" % min(16, len(d) // 4), d)
            print(f"  -- r8+{off:03x}: {len(d)} bytes; first ints {list(ints)[:12]}")
            print(f"       hex {d[:64].hex()}")

if __name__ == "__main__":
    main()
