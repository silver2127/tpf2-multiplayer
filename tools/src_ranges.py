"""Turn the sparse func->source map into contiguous ADDRESS RANGES per source file.

Only functions containing an assert get attributed directly (~6.5k of 138k). But
the linker lays out one translation unit's functions contiguously, so the first
and last attributed function of a .cpp bracket that whole object file. Sorting
all anchors by address and merging runs of the same file recovers the ranges --
which is enough to say "this unnamed function at 0x458xxx is streetbuilder.cpp"
without decompiling it.

Ranges are reported with the number of anchors behind them; a 1-anchor range is
a point, not evidence of extent. Interleaving (a foreign anchor inside another
file's span) is reported rather than smoothed over, because it means the linker
did not keep that object contiguous and the interpolation is unsafe there.

Usage: src_ranges.py [outdir] [filter-substring]
"""
import csv
import os
import sys

OUT = r"C:\tools\ghidra_out"
args = [a for a in sys.argv[1:]]
if args and os.path.isdir(args[0]):
    OUT = args.pop(0)
filt = args[0].lower() if args else ""

csv.field_size_limit(10 ** 7)
anchors = []  # (rva, file)
for r in csv.DictReader(open(os.path.join(OUT, "func2src.csv"))):
    files = r["source_files"].split(";")
    if len(files) != 1:
        continue  # inlined header code: ambiguous, not an anchor
    anchors.append((int(r["func_rva"], 16), files[0]))
anchors.sort()

runs = []
for rva, f in anchors:
    if runs and runs[-1][0] == f:
        runs[-1][2] = rva
        runs[-1][3] += 1
    else:
        runs.append([f, rva, rva, 1])

# merge runs of the same file that are separated only by other runs entirely
# inside them is NOT done: report raw, let the reader see interleaving.
best = {}
for f, lo, hi, n in runs:
    if f not in best or n > best[f][2]:
        best[f] = (lo, hi, n)

for f in sorted(best):
    if filt and filt not in f:
        continue
    lo, hi, n = best[f]
    total = sum(1 for _, x in anchors if x == f)
    print("%-60s %7x - %-7x  anchors=%d/%d" % (f, lo, hi, n, total))
