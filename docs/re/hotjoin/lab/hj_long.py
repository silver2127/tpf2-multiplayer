# Long-run check of the lab pair: every LSHASH stamp both sides published
# (full detail incl. the n: people lane), lane by lane, plus the person dumps.
import re, subprocess, sys
from pathlib import Path
r = Path('/opt/tpf2mp-linux-parity-20260921')
log = (r / 'lab/share/tpf2mp/data/tpf2_bridge.log').read_text(errors='replace')
mine, peer = {}, {}
for m in re.finditer(r'\[(tail\] sent|net\] peer) \(\d+ b\): LSHASH t=(\d+) h=(\S+) d=(\S+)', log):
    (mine if m[1].startswith('tail') else peer)[int(m[2])] = (m[3], m[4])
since = int(sys.argv[sys.argv.index('--since') + 1]) if '--since' in sys.argv else 0
com = sorted(t for t in mine.keys() & peer.keys() if t >= since)


def lanes(d):
    # v2,c5:x,e4544:x,z:x,p2@t:x,r0:x/x,m:-,l:-,t:4697,n:2384
    out = {}
    for part in re.split(r',(?=[a-z]\d*[:@]|[a-z]\d+,|v\d)', d):
        k = re.match(r'[a-z]+', part)
        out[k[0] if k else part] = part
    return out


bad = [t for t in com if mine[t] != peer[t]]
print(f'hash stamps compared: {len(com)} ({com[0] if com else "-"}..{com[-1] if com else "-"}), differing: {len(bad)}')
for t in bad[:6]:
    a, b = lanes(mine[t][1]), lanes(peer[t][1])
    diff = {k: (a.get(k), b.get(k)) for k in a.keys() | b.keys() if a.get(k) != b.get(k)}
    print(f'  t={t} verdict {"=" if mine[t][0] == peer[t][0] else "DIFF"} lanes {diff}')
subprocess.run(['python3', str(r / 'hj-lab/hj_compare.py')], stdout=None) if '--dumps' in sys.argv else None
