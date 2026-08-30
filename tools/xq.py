"""Query the pre-dumped static facts about TransportFever2.exe without touching
Ghidra. The Ghidra project is locked to one process, so every question answered
here is a serialised headless run avoided.

Facts it joins:
  call_edges.csv  direct call graph          (DumpCallEdges.java)
  func2src.csv    function -> __FILE__       (DumpStringXrefs.java + func2src.py)
  strings.csv     string -> referencing fns  (DumpStringXrefs.java)
  vtable_dump.csv class -> virtual slots     (DumpVtables.java)

Direct edges miss signals2/std::function/virtual dispatch, so "callers" is the
last hop only, never proof of what ultimately triggered something.

  xq.py callers <rva> [<rva>...]   who calls it, with source file
  xq.py callees <rva>              what it calls, with source file
  xq.py src <rva>                  which .cpp a function came from
  xq.py file <substr>              functions attributed to a source file
  xq.py str <substr>               strings matching, with referencing functions
  xq.py fnstr <rva>                strings referenced BY a function
"""
import bisect
import csv
import os
import sys

OUT = r"C:\tools\ghidra_out"
csv.field_size_limit(10 ** 7)


def load_f2s():
    m = {}
    p = os.path.join(OUT, "func2src.csv")
    for r in csv.DictReader(open(p)):
        m[int(r["func_rva"], 16)] = r["source_files"]
    return m


F2S = load_f2s()
KEYS = sorted(F2S)


def src(rva):
    """Exact attribution if the function itself has an assert; otherwise the
    nearest preceding attributed function, marked '~' because translation units
    are laid out contiguously but that is an inference, not a fact."""
    if rva in F2S:
        return F2S[rva]
    i = bisect.bisect_right(KEYS, rva) - 1
    if i < 0:
        return "?"
    return "~" + F2S[KEYS[i]]


def edges():
    for r in csv.reader(open(os.path.join(OUT, "call_edges.csv"))):
        if r[0] == "caller_rva":
            continue
        yield int(r[0], 16), int(r[1], 16), int(r[2], 16)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd = sys.argv[1]
    args = sys.argv[2:]

    if cmd == "src":
        for a in args:
            v = int(a, 16)
            print("%x  %s" % (v, src(v)))

    elif cmd == "callers":
        want = set(int(a, 16) for a in args)
        seen = set()
        for ca, ce, site in edges():
            if ce in want and (ca, ce) not in seen:
                seen.add((ca, ce))
                print("%-8x <- %-8x  %s" % (ce, ca, src(ca)))

    elif cmd == "callees":
        want = set(int(a, 16) for a in args)
        seen = set()
        for ca, ce, site in edges():
            if ca in want and (ca, ce) not in seen:
                seen.add((ca, ce))
                print("%-8x -> %-8x  %s" % (ca, ce, src(ce)))

    elif cmd == "file":
        sub = args[0].lower()
        for r in csv.DictReader(open(os.path.join(OUT, "src2func.csv"))):
            if sub in r["source_file"]:
                print("%s  n=%s" % (r["source_file"], r["nfuncs"]))
                print("   " + r["func_rvas"])

    elif cmd == "str":
        sub = args[0].lower()
        for r in csv.DictReader(open(os.path.join(OUT, "strings.csv"),
                                     encoding="utf-8", errors="replace")):
            if sub in (r["text"] or "").lower():
                print("%-9s n=%-3s %-40s  %s"
                      % (r["rva"], r["nrefs"], (r["ref_func_rvas"] or "")[:40], r["text"][:120]))

    elif cmd == "fnstr":
        want = set(a.lower().lstrip("0") for a in args)
        for r in csv.DictReader(open(os.path.join(OUT, "strings.csv"),
                                     encoding="utf-8", errors="replace")):
            fns = set(x.lstrip("0") for x in (r["ref_func_rvas"] or "").split())
            if fns & want:
                print("%-9s  %s" % (r["rva"], r["text"][:160]))
    else:
        print(__doc__)


main()
