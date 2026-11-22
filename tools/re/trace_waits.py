"""trace_waits.py -- where a game thread's wall time goes, from a trace_load.ps1 ETW
trace expanded to XML (tracerpt <etl> -o trace.xml -of XML).

RIP sampling (profile_load.py) sees only a thread that is RUNNING. A save load
whose loader thread runs 5-6 CPU-s per 10 s is waiting the rest of the time,
and sampling cannot say on what. The kernel's CSwitch and ReadyThread events
can: every switch-out carries the wait reason, and every wake-up names the
thread that readied it. Per thread of the game process this prints:

  * on-CPU time, and off-CPU time by wait reason (Executive, UserRequest, ...)
  * who readied it after each wait: another game thread (by tid), a thread of
    another process, or the idle/system context (I/O completion, timer)
  * a per-second timeline: running / waiting / ready-but-not-running

    python tools/re/trace_waits.py trace.xml [--pid N] [--threads 6] [--from S --to S]

Parsing is a streaming regex over the XML (the file is tens of GB for a
minute-long trace); expect a few minutes per 10 million events.
"""
import argparse
import collections
import re
import sys
from datetime import datetime

EVENT_END = '</Event>'
R_OPCODE = re.compile(r'<RenderingInfo[^>]*>.*?<Opcode>([^<]*)</Opcode>', re.S)
R_TIME = re.compile(r'<TimeCreated SystemTime="([^"]+)"')
R_EXEC = re.compile(r'<Execution ProcessID="(\d+)" ThreadID="(\d+)"')
R_DATA = re.compile(r'<Data Name="([^"]+)">([^<]*)</Data>')
R_PROC = re.compile(r'<Provider Name="([^"]*)"')

WAIT_REASONS = ['Executive', 'FreePage', 'PageIn', 'PoolAllocation', 'DelayExecution', 'Suspended',
                'UserRequest', 'WrExecutive', 'WrFreePage', 'WrPageIn', 'WrPoolAllocation', 'WrDelayExecution',
                'WrSuspended', 'WrUserRequest', 'WrEventPair', 'WrQueue', 'WrLpcReceive', 'WrLpcReply',
                'WrVirtualMemory', 'WrPageOut', 'WrRendezvous', 'WrKeyedEvent', 'WrTerminated', 'WrProcessInSwap',
                'WrCpuRateControl', 'WrCalloutStack', 'WrKernel', 'WrResource', 'WrPushLock', 'WrMutex',
                'WrQuantumEnd', 'WrDispatchInt', 'WrPreempted', 'WrYieldExecution', 'WrFastMutex', 'WrGuardedMutex',
                'WrRundown', 'WrAlertByThreadId', 'WrDeferredPreempt']


def ts(s):
    # 2026-09-28T05:08:54.123456700Z (tracerpt gives 100 ns digits)
    s = s.rstrip('Z')
    head, _, frac = s.partition('.')
    t = datetime.fromisoformat(head).timestamp()
    return t + (float('0.' + frac) if frac else 0.0)


def num(v):
    v = v.strip()
    return int(v, 16) if v.lower().startswith('0x') else int(v or 0)


def events(path):
    buf = ''
    with open(path, encoding='utf-8', errors='replace') as f:
        while True:
            chunk = f.read(1 << 24)
            if not chunk:
                break
            buf += chunk
            parts = buf.split(EVENT_END)
            buf = parts.pop()
            for p in parts:
                yield p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('xml')
    ap.add_argument('--pid', type=int, help='the game process (default: the process named TransportFever2)')
    ap.add_argument('--threads', type=int, default=6)
    ap.add_argument('--from', dest='t0', type=float, default=None, help='seconds from trace start')
    ap.add_argument('--to', dest='t1', type=float, default=None)
    a = ap.parse_args()

    tid_pid = {}
    names = {}                 # pid -> image name
    t_start = None
    running_since = {}         # tid -> t of switch-in
    wait_since = {}            # tid -> (t, reason)
    readied = {}               # tid -> (t, readier tid, readier pid)
    on_cpu = collections.Counter()
    off_cpu = collections.defaultdict(collections.Counter)      # tid -> reason -> seconds
    ready_wait = collections.Counter()                           # tid -> seconds ready but not yet running
    readiers = collections.defaultdict(collections.Counter)     # tid -> readier label -> seconds of wait ended
    timeline = collections.defaultdict(lambda: collections.defaultdict(lambda: [0.0, 0.0]))   # tid -> sec -> [run, wait]
    n = 0

    def window(t):
        return (a.t0 is None or t - t_start >= a.t0) and (a.t1 is None or t - t_start <= a.t1)

    def add_timeline(tid, t0, t1, idx):
        s = t0
        while s < t1:
            sec = int(s - t_start)
            e = min(t1, t_start + sec + 1)
            timeline[tid][sec][idx] += e - s
            s = e

    for ev in events(a.xml):
        n += 1
        if n % 2000000 == 0:
            print(f'  {n} events...', file=sys.stderr, flush=True)
        m = R_OPCODE.search(ev)
        op = m.group(1) if m else ''
        if op not in ('CSwitch', 'ReadyThread', 'Start', 'DCStart', 'Load', 'DCStart ', 'End'):
            if 'Process' not in ev[:400]:
                continue
        tm = R_TIME.search(ev)
        if not tm:
            continue
        t = ts(tm.group(1))
        if t_start is None:
            t_start = t
        data = dict(R_DATA.findall(ev))
        if op in ('Start', 'DCStart') and 'TThreadId' in data:
            tid_pid[num(data['TThreadId'])] = num(data.get('ProcessId', '0'))
            continue
        if op in ('Start', 'DCStart') and 'ImageFileName' in data:
            names[num(data.get('ProcessId', '0'))] = data['ImageFileName']
            continue
        if op == 'ReadyThread':
            ex = R_EXEC.search(ev)
            rt = num(data.get('TThreadId', '0'))
            if ex:
                readied[rt] = (t, int(ex.group(2)), int(ex.group(1)))
            continue
        if op == 'CSwitch':
            new, old = num(data.get('NewThreadId', '0')), num(data.get('OldThreadId', '0'))
            reason = data.get('OldThreadWaitReason', '?')
            try:
                reason = WAIT_REASONS[int(reason)]
            except (ValueError, IndexError):
                pass
            # old thread leaves the CPU
            if old in running_since:
                s = running_since.pop(old)
                if window(t):
                    on_cpu[old] += t - s
                    add_timeline(old, s, t, 0)
            wait_since[old] = (t, reason)
            # new thread takes it
            running_since[new] = t
            if new in wait_since:
                ws, r = wait_since.pop(new)
                if window(t):
                    off_cpu[new][r] += t - ws
                    add_timeline(new, ws, t, 1)
                    rd = readied.pop(new, None)
                    if rd and rd[0] >= ws:
                        rpid = rd[2] or tid_pid.get(rd[1], -1)
                        label = ('tid %d' % rd[1]) if rpid == tid_pid.get(new) else ('%s (pid %d)' % (names.get(rpid, 'idle/system' if rpid in (0, 4) else '?'), rpid))
                        readiers[new][label] += t - ws
                        ready_wait[new] += t - rd[0]

    pid = a.pid or next((p for p, nm in names.items() if 'TransportFever2' in nm), None)
    print(f'{n} events; game pid {pid} ({names.get(pid)})')
    game = [tid for tid in set(on_cpu) | set(off_cpu) if tid_pid.get(tid) == pid]
    game.sort(key=lambda t: -on_cpu[t])
    for tid in game[:a.threads]:
        waited = sum(off_cpu[tid].values())
        print(f'\n=== tid {tid}: on CPU {on_cpu[tid]:.1f} s, off CPU {waited:.1f} s (of which ready-not-running {ready_wait[tid]:.2f} s)')
        for r, s in off_cpu[tid].most_common(6):
            print(f'      wait {r:<20} {s:8.2f} s')
        for who, s in readiers[tid].most_common(6):
            print(f'      woken by {who:<28} after {s:8.2f} s of waiting')
        secs = sorted(timeline[tid])
        if secs:
            line = ' '.join(f'{int(100 * timeline[tid][s][0])}' for s in range(secs[0], secs[-1] + 1))
            print(f'      % running per second from t+{secs[0]}s: {line}')


if __name__ == '__main__':
    main()
