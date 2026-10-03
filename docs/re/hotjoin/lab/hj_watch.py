# Emit a line every `every` checks, and at once when hashes or dumps start to differ
# or a game dies. usage: hj_watch.py [period_s] [report_every]
import re, subprocess, sys, time
from pathlib import Path
r = Path('/opt/tpf2mp-linux-parity-20260921')
period = int(sys.argv[1]) if len(sys.argv) > 1 else 60
every = int(sys.argv[2]) if len(sys.argv) > 2 else 5
last_bad = None
n = 0
while True:
    n += 1
    for u in ('tpf2mp-retain-lab', 'tpf2mp-retain-peer'):
        if subprocess.run(['systemctl', 'is-active', '-q', u]).returncode:
            print(f'DEAD {u}', flush=True); raise SystemExit(1)
    st = {}
    for nm, i in (('lab', 'a'), ('peer', 'b')):
        try:
            st[nm] = (r / nm / f'share/tpf2mp/data/lockstep_status_{i}.txt').read_text().split('\n')[0]
        except Exception:
            st[nm] = '?'
    h = subprocess.run(['python3', str(r / 'hj-lab/hj_long.py')], capture_output=True, text=True).stdout.strip()
    c = subprocess.run(['python3', str(r / 'hj-lab/hj_compare.py')], capture_output=True, text=True).stdout
    c += subprocess.run(['python3', str(r / 'hj-lab/hj_compare.py'), 'hjveh'], capture_output=True, text=True).stdout
    rows = re.findall(r'^t=(\S+) .* rowdiffs=(\d+)', c, re.M)
    badrows = [t for t, d in rows if d != '0']
    hd = re.search(r'differing: (\d+)', h)
    bad = (hd[1] if hd else '?', len(badrows))
    head = h.splitlines()[0] if h else ''
    line = f'{st["lab"][:40]} | peer {st["peer"][:28]} | {head} | dumps {len(rows)} rowdiff-samples {len(badrows)}'
    if bad != last_bad and (bad[0] not in ('0', '?') or bad[1]):
        print('DIFFERENCE: ' + line, flush=True)
        print('\n'.join(h.splitlines()[1:4]), flush=True)
        if badrows: print('first differing dump at t=' + badrows[0], flush=True)
    elif n % every == 1:
        print(line, flush=True)
    last_bad = bad
    time.sleep(period)
