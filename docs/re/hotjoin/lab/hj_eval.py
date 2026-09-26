# Append a Lua file as one EVAL line to the lab pair's inject files.
# usage: hj_eval.py chunk.lua [lab|peer|both]
import sys, re
from pathlib import Path
r = Path('/opt/tpf2mp-linux-parity-20260921')
src = Path(sys.argv[1]).read_text()
lines = []
for ln in src.splitlines():
    s = ln.strip()
    if not s or s.startswith('--'):
        continue
    lines.append(s)
chunk = ' '.join(lines)
which = sys.argv[2] if len(sys.argv) > 2 else 'both'
for n, i in (('lab', 'a'), ('peer', 'b')):
    if which in (n, 'both'):
        p = r / n / f'share/tpf2mp/data/lockstep_inject_{i}.txt'
        with p.open('a') as f:
            f.write('EVAL ' + chunk + '\n')
        print('queued', len(chunk), 'bytes ->', p)
