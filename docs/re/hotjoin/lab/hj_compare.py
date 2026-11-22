# Compare hjprobe dumps of the lab pair at equal sim times.
import re, sys
from pathlib import Path
r = Path('/opt/tpf2mp-linux-parity-20260921')
da, db = r / 'lab/share/tpf2mp/data', r / 'peer/share/tpf2mp/data'
prefix = sys.argv[1] if len(sys.argv) > 1 else 'hjprobe'
maxshow = int(sys.argv[2]) if len(sys.argv) > 2 else 12


def load(p):
    lines = p.read_text().splitlines()
    order = lines[0][6:].split(',') if lines and lines[0].startswith('ORDER ') else []
    rows = {}
    for ln in lines[1:]:
        if ln:
            rows[ln.split('|', 1)[0]] = ln
    return order, rows


ta = {float(m[1]): p for p in da.glob(prefix + '_a_*.txt') if (m := re.search(r'_a_([\d.]+)\.txt$', p.name))}
tb = {float(m[1]): p for p in db.glob(prefix + '_b_*.txt') if (m := re.search(r'_b_([\d.]+)\.txt$', p.name))}
common = sorted(ta.keys() & tb.keys())
print(f'host dumps {len(ta)}, peer dumps {len(tb)}, common {len(common)}: {common[:1]}..{common[-1:]}')
shown = 0
for t in common:
    oa, ra = load(ta[t])
    ob, rb = load(tb[t])
    same_set = set(oa) == set(ob)
    same_order = oa == ob
    diff = [k for k in sorted(ra.keys() | rb.keys(), key=int) if ra.get(k) != rb.get(k)]
    status = f't={t:.1f} n={len(oa)}/{len(ob)} set={"=" if same_set else "DIFF"} order={"=" if same_order else "DIFF"} rowdiffs={len(diff)}'
    print(status)
    if not same_order and same_set and shown < 2:
        first = next(i for i, (x, y) in enumerate(zip(oa, ob)) if x != y)
        print('   first order diff at index', first, 'host', oa[first:first + 6], 'peer', ob[first:first + 6])
    if diff and shown < 3:
        shown += 1
        for k in diff[:maxshow]:
            print('   A', ra.get(k, '<absent>'))
            print('   B', rb.get(k, '<absent>'))
