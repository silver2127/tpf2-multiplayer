"""Map FUNCTION RVA -> SOURCE FILE, from the __FILE__ strings that assert macros
compile into a release build.

The binary is stripped: of 76,873 "named" functions, 69,880 are Unwind metadata,
so there is no symbol table to read. But every assert() carries __FILE__, and the
function that references "...\\game\\ui\\actions\\streetbuilder.cpp" is, by
construction, a function compiled from streetbuilder.cpp. That turns ~700 file
paths into a partial symbol table over several thousand functions -- which is
what tells a UI tool apart from a simulation system without decompiling first.

Input : C:\\tools\\ghidra_out\\strings.csv   (from DumpStringXrefs.java)
Output: C:\\tools\\ghidra_out\\func2src.csv  (func_rva,source_files)
        C:\\tools\\ghidra_out\\src2func.csv  (source_file,nfuncs,func_rvas)
"""
import collections
import csv
import os
import re
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else r"C:\tools\ghidra_out"
csv.field_size_limit(10 ** 7)

rows = list(csv.DictReader(open(os.path.join(OUT, "strings.csv"),
                                encoding="utf-8", errors="replace")))

SRC = re.compile(r"\.(cpp|h|hpp|inl)$", re.I)
STRIP = re.compile(r".*?(?:train_fever|gamelib)[\\/]src[\\/]", re.I)

f2s = collections.defaultdict(set)
s2f = collections.defaultdict(set)
n = 0
for r in rows:
    t = (r["text"] or "").strip()
    if not SRC.search(t) or "urban_games" not in t:
        continue
    if "third_party" in t.lower():
        continue
    name = STRIP.sub("", t).lower().replace("/", "\\")
    n += 1
    for fn in (r["ref_func_rvas"] or "").split():
        if fn.startswith("@"):
            continue
        f2s[fn].add(name)
        s2f[name].add(fn)

with open(os.path.join(OUT, "func2src.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["func_rva", "source_files"])
    for fn in sorted(f2s, key=lambda x: int(x, 16)):
        w.writerow([fn, ";".join(sorted(f2s[fn]))])

with open(os.path.join(OUT, "src2func.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["source_file", "nfuncs", "func_rvas"])
    for s in sorted(s2f):
        w.writerow([s, len(s2f[s]), " ".join(sorted(s2f[s], key=lambda x: int(x, 16)))])

print("file strings: %d   functions attributed: %d   distinct sources: %d"
      % (n, len(f2s), len(s2f)))
