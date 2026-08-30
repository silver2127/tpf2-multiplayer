#!/usr/bin/env python3
"""Diff the hook's DUMPPROP captures: UI construction placement vs Lua replay.

Reads a tpf2_slice.log, reassembles the last D8u_/D8l_ (r8 = proposal) and
D9u_/D9l_ (r9) hex dumps, and reports every 8-byte slot that differs, tagged
as pointer-like (both values look like heap addresses -- expected to differ)
or VALUE (small ints / floats -- the interesting ones). Also lists the DVEC
vector summaries for each side so vector counts can be compared.

    python tools/dumpprop_diff.py [path\\to\\tpf2_slice.log] [-b]
      -b  use the sandboxed (instance B) log
"""
import re, struct, sys

REAL = r"C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out\tpf2_slice.log"
OVL  = r"C:\Sandbox\" + os.environ.get("USERNAME", "user") + r"\GameAgent\drive\C\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out\tpf2_slice.log"

def load(path):
    with open(path, "r", errors="replace") as f:
        return f.read().splitlines()

def all_dumps(lines, tag):
    """Every complete dump with this tag (e.g. 'D8u_'), in log order."""
    pat = re.compile(r"\[gt\] " + re.escape(tag) + r"0\.0\+([0-9a-f]{3}):([0-9a-f]+)")
    dumps, cur = [], {}
    for ln in lines:
        m = pat.search(ln)
        if not m: continue
        off = int(m.group(1), 16)
        if off == 0 and cur: dumps.append(cur); cur = {}
        cur[off] = bytes.fromhex(m.group(2))
    if cur: dumps.append(cur)
    out = []
    for d in dumps:
        b = bytearray()
        for off in sorted(d): b += d[off]
        out.append(bytes(b))
    return out

def pick(lines, tag, idx):
    """idx: -1 = last (default), else 0-based index into the dump list."""
    ds = all_dumps(lines, tag)
    if not ds: return None
    if idx < 0 or idx >= len(ds): idx = len(ds) - 1
    return ds[idx]

def looks_ptr(v):
    return 0x10000 <= v < 0x7FFFFFFFFFFF

def diff(a, b, name):
    n = min(len(a), len(b))
    print(f"== {name}: {len(a)} vs {len(b)} bytes, comparing {n}")
    for off in range(0, n - 7, 8):
        qa = struct.unpack_from("<Q", a, off)[0]
        qb = struct.unpack_from("<Q", b, off)[0]
        if qa == qb: continue
        kind = "ptr " if (looks_ptr(qa) and looks_ptr(qb)) else "VALUE"
        fa = struct.unpack_from("<2f", a, off)
        fb = struct.unpack_from("<2f", b, off)
        print(f"  +{off:03x} {kind} ui={qa:016x} lua={qb:016x}   f32 ui=({fa[0]:.4g},{fa[1]:.4g}) lua=({fb[0]:.4g},{fb[1]:.4g})")

def vecs(lines, c):
    pat = re.compile(r"\[slice\] DVEC c=%d r8\+([0-9a-f]{3}) span=(\d+)" % c)
    seen = {}
    for ln in lines:
        m = pat.search(ln)
        if m: seen[int(m.group(1), 16)] = int(m.group(2))
    return seen

def main():
    path = OVL if "-b" in sys.argv else REAL
    ui_i, lua_i = -1, -1
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "-u": ui_i = int(args[i + 1])
        elif a == "-l": lua_i = int(args[i + 1])
        elif a not in ("-b",) and not (i and args[i - 1] in ("-u", "-l")): path = a
    lines = load(path)
    nu, nl = len(all_dumps(lines, "D8u_")), len(all_dumps(lines, "D8l_"))
    print(f"dumps in log: UI={nu} Lua={nl}  (choose with -u N / -l N, 0-based; default last)")
    u8, l8 = pick(lines, "D8u_", ui_i), pick(lines, "D8l_", lua_i)
    u9, l9 = pick(lines, "D9u_", ui_i), pick(lines, "D9l_", lua_i)
    if not u8 or not l8:
        print("need both a UI (D8u_) and a Lua (D8l_) dump in", path); return
    diff(u8, l8, "r8 proposal")
    if u9 and l9: diff(u9, l9, "r9")
    vu, vl = vecs(lines, 1), vecs(lines, 2)
    print("== vectors in r8 (offset: span bytes)")
    for off in sorted(set(vu) | set(vl)):
        print(f"  +{off:03x} ui={vu.get(off, '-')} lua={vl.get(off, '-')}")

if __name__ == "__main__":
    main()
