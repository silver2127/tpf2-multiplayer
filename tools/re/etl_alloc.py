r"""etl_alloc.py -- who commits memory during a load, from a tracerpt CSV.

Input: the CSV that `tracerpt <trace>.etl -o out.csv -of CSV` writes from a
trace_load.ps1 -FileMode capture (VirtualAllocation provider with stacks).

For one process it takes every PageFault/VirtualAlloc event whose Flags
include MEM_COMMIT (0x1000), pairs it with the StackWalk event that follows
it for the same process and thread, and keys it by the first game-module
frames of that stack. Every VirtualFree with MEM_RELEASE or MEM_DECOMMIT is
matched to the allocation at the same base address, so the report gives, per
call site, the bytes committed in total, the bytes alive at the moment the
process's live total peaked, and the bytes still alive at the end.

Heap growth goes through NtAllocateVirtualMemory too, so the game code that
pushed the heap into a new segment is on those stacks; a Segment Heap block
above 128 KB is a VirtualAlloc of its own with the exact (64 KB-rounded)
size, which is how the per-tile buffers show up one by one.

Row layout (tracerpt, Windows 11): Event Name, Type, ..., PID (hex), TID
(hex), ..., Clock-Time, Kernel(ms), User(ms), then the event's own fields:
  VirtualAlloc/Free: BaseAddress, RegionSize, ProcessId (dec), Flags
  StackWalk:         EventTimeStamp, StackProcess (hex), StackThread (dec), frames
  Image:             ImageBase, ImageSize, ProcessId, ..., FileName (last field)

    python tools\re\etl_alloc.py load.csv --pid 57708 [--frames 3] [--top 40]
"""
import argparse, sys, collections, time

def pint(s):
    s = s.strip()
    if not s: return 0
    try:
        return int(s, 16) if s[:2].lower() == '0x' else int(s)
    except ValueError:
        return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv'); ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--frames', type=int, default=3); ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--window', type=int, default=400, help='max rows between an allocation and its stack')
    ap.add_argument('--exe', default='transportfever2.exe')
    a = ap.parse_args()
    pid = a.pid
    modules = []
    pending = {}                      # tid -> (base, size, row, sec)
    live = {}                         # base -> (size, key)
    total = collections.Counter(); cnt = collections.Counter()
    livekey = collections.Counter()   # key -> live bytes now
    live_total = 0; peak_total = 0; peak_sec = ''; peak_snapshot = None
    timeline = collections.Counter(); freed = collections.Counter()
    n = 0; t0 = time.time(); unmatched_free = 0

    def key_of(cols):
        frames = []
        for c in cols[22:]:
            addr = pint(c)
            if not addr or addr >= 0xFFFF800000000000: continue
            for base, msize, mname in modules:
                if base <= addr < base + msize:
                    if mname == a.exe: frames.append('exe+%x' % (addr - base))
                    elif not mname.startswith(('ntdll', 'kernel', 'ucrt', 'vcruntime', 'msvcp', 'win32u', 'gdi32', 'user32')):
                        frames.append('%s+%x' % (mname, addr - base))
                    break
            if len(frames) >= a.frames: break
        return ' < '.join(frames) if frames else '(no game frame)'

    def commit(base, size, key, sec):
        nonlocal live_total, peak_total, peak_sec, peak_snapshot
        total[key] += size; cnt[key] += 1; timeline[sec] += size
        if base in live:            # re-commit inside a region: count the growth only
            osize, okey = live[base]
            livekey[okey] -= osize; live_total -= osize
        live[base] = (size, key); livekey[key] += size; live_total += size
        if live_total > peak_total:
            peak_total = live_total; peak_sec = sec; peak_snapshot = None

    with open(a.csv, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            n += 1
            if n % 20000000 == 0: print(f'... {n} rows, {time.time() - t0:.0f} s', file=sys.stderr)
            s = line.lstrip()
            if s.startswith('PageFault, VirtualAlloc,') or s.startswith('PageFault, VirtualFree,'):
                cols = line.split(',')
                if len(cols) < 23 or pint(cols[21]) != pid: continue
                base = pint(cols[19]); size = pint(cols[20]); flags = pint(cols[22]); tid = pint(cols[10])
                sec = cols[16].strip()[:-7]
                if cols[1].strip() == 'VirtualAlloc':
                    if not flags & 0x1000: continue
                    # A stack may never come (window); attribute then as unknown.
                    old = pending.pop(tid, None)
                    if old: commit(old[0], old[1], '(no stack)', old[3])
                    pending[tid] = (base, size, n, sec)
                else:
                    if not flags & 0xC000: continue
                    freed[sec] += size
                    v = live.pop(base, None)
                    if v:
                        osize, okey = v; livekey[okey] -= osize; live_total -= osize
                    else:
                        unmatched_free += 1
            elif s.startswith('StackWalk,'):
                if not pending: continue
                cols = line.split(',')
                if len(cols) < 23 or pint(cols[20]) != pid: continue
                p = pending.pop(pint(cols[21]), None)
                if not p: continue
                base, size, row, sec = p
                commit(base, size, key_of(cols) if n - row <= a.window else '(stack too far)', sec)
            elif s.startswith('Image,'):
                cols = line.split(',')
                if len(cols) < 22 or cols[1].strip() not in ('DCStart', 'Load') or pint(cols[21]) != pid: continue
                fname = next((c.strip() for c in reversed(cols) if c.strip()), '')
                modules.append((pint(cols[19]), pint(cols[20]), fname.split('\\')[-1].lower()))
            # Snapshot the per-key live set at the peak lazily: the first row after
            # a new peak captures it (cheap enough at a few hundred keys).
            if peak_snapshot is None and peak_total:
                peak_snapshot = dict(livekey)
    for tid, (base, size, row, sec) in list(pending.items()): commit(base, size, '(no stack)', sec)
    print(f'{n} rows in {time.time() - t0:.0f} s; pid {pid}: committed {sum(total.values()) / 2**30:.2f} GB in total; '
          f'peak live {peak_total / 2**30:.2f} GB at second {peak_sec}; still live at the end {live_total / 2**30:.2f} GB; unmatched frees {unmatched_free}')
    print('--- top call sites: live at the peak / committed in total / still live at the end (MB), allocations, key (innermost first)')
    rows = sorted(total, key=lambda k: -(peak_snapshot or {}).get(k, 0))
    for k in rows[:a.top]:
        print(f'{(peak_snapshot or {}).get(k, 0) / 2**20:10.1f} {total[k] / 2**20:10.1f} {livekey[k] / 2**20:10.1f} {cnt[k]:8d}  {k}')
    print('--- per second: committed / released / live at end of second (MB)')
    running = 0
    for t in sorted(set(timeline) | set(freed)):
        c = timeline.get(t, 0); r = freed.get(t, 0); running += c - r
        if c or r: print(f'{t}  +{c / 2**20:8.0f}  -{r / 2**20:8.0f}  ={running / 2**20:8.0f}')

if __name__ == '__main__': main()
