"""trace_stacks.py -- per-thread CPU profile with call stacks from an ETW trace
(trace_load.ps1 -Light) expanded to XML by tracerpt.

The -Light capture has no context switches, but it has the kernel's CPU sampler
(PerfInfo/SampleProf) with a StackWalk after every sample. tracerpt cannot
decode StackWalk (ProcessingErrorData), so its payload is read here:
    EventTimeStamp u64, StackProcess u32, StackThread u32, then u64 frames
(return addresses, innermost first; kernel frames first when the sample hit
kernel mode). Frames inside TransportFever2.exe are named by their function's
start (.pdata), like profile_load.py.

    python tools/re/trace_stacks.py trace.xml [--threads 4] [--top 30] [--per-second]
"""
import argparse
import bisect
import collections
import re
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from profile_load import pdata, EXE_PATH   # noqa: E402

R_PAYLOAD = re.compile(r'<EventPayload>([0-9A-Fa-f]+)</EventPayload>')
R_DATA = re.compile(r'<Data Name="([^"]+)">([^<]*)</Data>')
R_TIME = re.compile(r'SystemTime="\d{4}-\d\d-\d\dT(\d\d):(\d\d):(\d\d)\.(\d+)')


def events(path):
    buf = ''
    with open(path, encoding='utf-8', errors='replace') as f:
        while True:
            chunk = f.read(1 << 24)
            if not chunk:
                break
            buf += chunk
            parts = buf.split('</Event>')
            buf = parts.pop()
            yield from parts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('xml')
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--depth', type=int, default=4, help='exe frames per chain in the chain table')
    a = ap.parse_args()

    exe_ib, fn = pdata(EXE_PATH)
    starts = [int(x) for x in fn['b']]
    ends = [int(x) for x in fn['e']]
    images = {}                       # pid -> [(base, end, name)]
    game_pid, game_base = None, None
    per_tid = collections.Counter()
    tid_pid = {}
    incl = collections.defaultdict(collections.Counter)    # tid -> function -> samples
    leaf = collections.defaultdict(collections.Counter)
    chains = collections.defaultdict(collections.Counter)
    secs = collections.defaultdict(collections.Counter)    # tid -> second -> samples
    t0 = None
    n = 0

    def name(pid, addr):
        if game_base and game_base <= addr < game_base + 0x6000000:
            rva = addr - game_base
            i = bisect.bisect_right(starts, rva) - 1
            if i >= 0 and rva < ends[i]:
                return 'exe!0x%x' % (exe_ib + starts[i])
            return 'exe!?'
        if addr >= 0xFFFF000000000000:
            return 'kernel'
        for b, e, nm in images.get(pid, ()):
            if b <= addr < e:
                return nm
        return '?'

    # A process already running when the trace began has no Image/Load events;
    # its modules are only in the rundown (DCEnd) at the END of the trace.
    import os
    with open(a.xml, 'rb') as f:
        f.seek(max(0, os.path.getsize(a.xml) - 300_000_000))
        tail = f.read().decode('utf-8', 'replace')
    for ev in tail.split('</Event>'):
        if 'ImageBase' not in ev or 'FileName' not in ev:
            continue
        d = dict(R_DATA.findall(ev))
        pid = int(d.get('ProcessId', '0').strip() or 0)
        base, size = int(d['ImageBase'], 16), int(d['ImageSize'], 16)
        nm = d['FileName'].rsplit(chr(92), 1)[-1]
        images.setdefault(pid, []).append((base, base + size, nm))
        if nm.lower() == 'transportfever2.exe' and pid:
            game_pid, game_base = pid, base
    del tail
    print(f'rundown: game pid {game_pid}, exe base {hex(game_base or 0)}, {len(images.get(game_pid, []))} modules', file=sys.stderr, flush=True)

    for ev in events(a.xml):
        n += 1
        if n % 2000000 == 0:
            print(f'  {n} events...', file=sys.stderr, flush=True)
        if '<Opcode>Load</Opcode>' in ev or '<Opcode>DCStart</Opcode>' in ev and 'ImageBase' in ev:
            d = dict(R_DATA.findall(ev))
            if 'ImageBase' in d and 'FileName' in d:
                pid = int(d.get('ProcessId', '0').strip() or 0)
                base, size = int(d['ImageBase'], 16), int(d['ImageSize'], 16)
                nm = d['FileName'].rsplit('\\', 1)[-1]
                images.setdefault(pid, []).append((base, base + size, nm))
                if nm.lower() == 'transportfever2.exe':
                    game_pid, game_base = pid, base
            continue
        if '<Opcode>Stack</Opcode>' not in ev or game_pid is None:
            continue
        m = R_PAYLOAD.search(ev)
        if not m:
            continue
        raw = bytes.fromhex(m.group(1))
        if len(raw) < 24:
            continue
        stamp, spid, stid = struct.unpack_from('<QII', raw, 0)
        if spid != game_pid:
            continue
        frames = struct.unpack_from('<%dQ' % ((len(raw) - 16) // 8), raw, 16)
        if t0 is None:
            t0 = stamp
        tm = R_TIME.search(ev)
        sec = 0
        if tm:
            h, mi, s_ = int(tm.group(1)), int(tm.group(2)), int(tm.group(3))
            sec = h * 3600 + mi * 60 + s_
        per_tid[stid] += 1
        names = [name(spid, f) for f in frames]
        user = [x for x in names if x != 'kernel']
        lf = user[0] if user else 'kernel'
        leaf[stid][lf] += 1
        for x in set(names):
            incl[stid][x] += 1
        ex = [x for x in user if x.startswith('exe!')][:a.depth]
        chains[stid][(lf,) + tuple(ex)] += 1
        secs[stid][sec] += 1

    print(f'{n} events; game pid {game_pid}, exe base {hex(game_base or 0)}')
    for tid, c in per_tid.most_common(a.threads):
        print(f'\n=== tid {tid}: {c} samples')
        s = secs[tid]
        if s:
            lo, hi = min(s), max(s)
            print('    samples per second from', lo % 86400, ':', ' '.join(str(s.get(x, 0)) for x in range(lo, hi + 1)))
        print('    --- inclusive ---')
        for f, k in incl[tid].most_common(a.top):
            print(f'    {k:7d} {100 * k / c:5.1f}%  {f}')
        print('    --- leaf ---')
        for f, k in leaf[tid].most_common(12):
            print(f'    {k:7d} {100 * k / c:5.1f}%  {f}')
        print('    --- chains ---')
        for ch, k in chains[tid].most_common(15):
            print(f'    {k:7d}  ' + ' <- '.join(ch))


if __name__ == '__main__':
    main()
