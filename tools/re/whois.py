"""Name an arbitrary function RVA from the pre-dumped static facts.

Three independent sources, reported separately so the reader can see which one
is carrying the claim:

  sig   exact __FUNCSIG__ referenced by that function  -- strongest, a real name
  cpp   nearest preceding .cpp anchor (__FILE__)       -- a translation unit
  vt    vftable class + slot, if the address is in one -- names the role

Header anchors (lib\\util\\cast.h and friends) are skipped when locating the
.cpp, because inlined template code from a header attaches to whatever
translation unit inlined it and would otherwise mask the real file.

Usage: whois.py <rva> [<rva>...]
"""
import bisect
import csv
import os
import sys

OUT = r"C:\tools\ghidra_out"
csv.field_size_limit(10 ** 7)

sig = {}
for r in csv.DictReader(open(os.path.join(OUT, "funcsig.csv"), encoding="utf-8")):
    sig[int(r["func_rva"], 16)] = r["signatures"].split(" || ")

cpp = {}
for r in csv.DictReader(open(os.path.join(OUT, "func2src.csv"))):
    files = [f for f in r["source_files"].split(";") if f.endswith(".cpp")]
    if len(files) == 1:
        cpp[int(r["func_rva"], 16)] = files[0]
ck = sorted(cpp)

vt = {}
for r in csv.DictReader(open(os.path.join(OUT, "vtable_dump.csv"))):
    vt.setdefault(int(r["target_rva"], 16), []).append(
        "%s slot %s" % (r["class"], r["slot"]))

for a in sys.argv[1:]:
    v = int(a.replace("0x", ""), 16)
    print("== %x" % v)
    if v in sig:
        for s in sig[v]:
            print("   sig  %s" % s)
    i = bisect.bisect_right(ck, v) - 1
    if i >= 0:
        exact = "" if cpp.get(v) else "~"
        print("   cpp  %s%s   (anchor %x)" % (exact, cpp[ck[i]], ck[i]))
    if v in vt:
        for x in vt[v][:8]:
            print("   vt   %s" % x)
    if v not in sig and i < 0 and v not in vt:
        print("   (nothing known)")
