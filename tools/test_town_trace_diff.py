"""tools/town_trace_diff.py classifies the first differences correctly.

    python tools/test_town_trace_diff.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def tt(t, town, i, lst, mt0, mt1, n=4, e=0):
    return f'TT t={t} town={town} i={i} n={n} list={lst:08x} mt0={mt0:016x} mt1={mt1:016x} e={e}\n'


def run(a, b):
    with tempfile.TemporaryDirectory() as d:
        pa, pb = Path(d) / 'a.txt', Path(d) / 'b.txt'
        pa.write_text(a)
        pb.write_text(b)
        r = subprocess.run([sys.executable, str(HERE / 'town_trace_diff.py'), str(pa), str(pb)],
                           capture_output=True, text=True)
        return r.returncode, r.stdout


head = '# town trace, native build 35924, format 1\n'
same = tt(1200, 11, 0, 0xaaaa, 1, 2) + tt(1200, 22, 1, 0xaaaa, 2, 3) + tt(1204, 33, 2, 0xaaaa, 9, 9)
code, out = run(head + same, head + same)
assert code == 0 and 'every Develop call in the common window agrees' in out, out

# Town list order differs: named as node-list order.
code, out = run(head + same, head + tt(1200, 11, 3, 0xbbbb, 1, 2) + tt(1200, 22, 1, 0xbbbb, 2, 3) + tt(1204, 33, 2, 0xbbbb, 9, 9))
assert code == 1 and 't=1200 (240.0 units) town=11: Town node list differs' in out and 'node-list order' in out, out

# Same list, same generator in, different generator out: this town decided.
code, out = run(head + same, head + tt(1200, 11, 0, 0xaaaa, 1, 2) + tt(1200, 22, 1, 0xaaaa, 2, 7) + tt(1204, 33, 2, 0xaaaa, 9, 9))
assert 'town=22: same inputs, generator differs on exit: THIS town decided differently' in out, out
assert 'town=11' not in out and 'town=33' not in out, out

# Generator differs on entry: an earlier town of the tick.
code, out = run(head + same, head + tt(1200, 11, 0, 0xaaaa, 1, 2) + tt(1200, 22, 1, 0xaaaa, 5, 3) + tt(1204, 33, 2, 0xaaaa, 9, 9))
assert 'town=22: generator differs on entry' in out, out

# Developed on one side only; outside the common window is ignored.
code, out = run(head + tt(1000, 5, 0, 1, 1, 1) + same, head + tt(1200, 11, 0, 0xaaaa, 1, 2) + tt(1204, 33, 2, 0xaaaa, 9, 9))
assert 'town=22: developed on A only' in out and 'town=5' not in out, out

# TF: differing tokens reported.
tfa = 'TF t=1200 e=0 lists=2 3:00000001 5:00000002\n'
tfb = 'TF t=1200 e=1 lists=2 3:00000001 5:00000009\n'
code, out = run(head + same + tfa, head + same + tfb)
assert "TF window t=1200: node lists differ" in out and "5:00000002" in out and "5:00000009" in out, out
print('town_trace_diff: agreement, list order, this-town, earlier-town, one-sided, window, TF: passed')
