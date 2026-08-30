"""Recover a partial SYMBOL TABLE from __FUNCSIG__ strings.

The release build is stripped -- of 76,873 "named" functions 69,880 are Unwind
metadata -- but MSVC's assert/verify macros expand __FUNCSIG__, which bakes the
FULL demangled signature of the enclosing function into .rdata as a literal:

    struct Command __cdecl make_cmd::BuyVehicle(const class ecs::Engine &, ...)

and the only code that references that literal is that same function. So a
string xref gives function RVA -> real C++ name, arguments and return type. This
is strictly stronger than the __FILE__ map (which only gives the .cpp), and it
is what turns "FUN_1409dca00 fires once per purchase" into
"make_cmd::BuyVehicle" without decompiling anything.

Caveat worth keeping: a signature can be referenced by an inlined copy or by a
wrapper, so an entry with several distinct signatures is ambiguous and is marked.

Input : C:\\tools\\ghidra_out\\strings.csv
Output: C:\\tools\\ghidra_out\\funcsig.csv  -- func_rva,nsigs,signature(s)
"""
import collections
import csv
import os
import re
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else r"C:\tools\ghidra_out"
csv.field_size_limit(10 ** 7)

# A signature string, not prose: has a calling convention and a parameter list.
SIG = re.compile(r"__(cdecl|thiscall|stdcall|fastcall|vectorcall)\b")

sigs = collections.defaultdict(set)
n = 0
for r in csv.DictReader(open(os.path.join(OUT, "strings.csv"),
                             encoding="utf-8", errors="replace")):
    t = (r["text"] or "").strip()
    if not SIG.search(t) or "(" not in t:
        continue
    n += 1
    for f in (r["ref_func_rvas"] or "").split():
        if f.startswith("@"):
            continue
        sigs[int(f, 16)].add(t)

with open(os.path.join(OUT, "funcsig.csv"), "w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh)
    w.writerow(["func_rva", "nsigs", "signatures"])
    for f in sorted(sigs):
        w.writerow(["%x" % f, len(sigs[f]), " || ".join(sorted(sigs[f]))])

print("signature strings: %d   functions named: %d   unambiguous: %d"
      % (n, len(sigs), sum(1 for v in sigs.values() if len(v) == 1)))
