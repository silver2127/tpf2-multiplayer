"""profile_load.py -- sampling profiler for TransportFever2 save loads.

Answers "where does a big save load actually spend its time" with a measurement
instead of an argument. Samples every thread's instruction pointer on a timer,
resolves each sample to a module and (inside the exe) to the containing function
via .pdata, and prints a histogram.

WHY RIP AND NOT A STACK WALK
A raw stack scan reports stale return addresses left over from earlier calls --
this project already chased a phantom TownDeveloper::Develop frame that way.
RIP is where the CPU actually is, with no unwinding and no ambiguity. The cost
is that you see leaves, not callers: if everything is in a lock you learn that
it is a lock, but not who holds it. That is still the question worth first.

The work/wait split matters as much as the histogram. A thread parked in
RtlSleepConditionVariableSRW is idle, and counting those as "time spent" is how
you conclude the game is busy allocating when it is really waiting. Samples in
known ntdll wait primitives are tallied separately and reported apart.

Usage:
    python tools/profile_load.py --pid 1234 --seconds 900
    python tools/profile_load.py --wait-for-game --seconds 1800   (busiest instance)
    python tools/profile_load.py --all --seconds 1800             (every instance, per-pid)

Start it BEFORE hitting Load. Windowed snapshots (--window) keep menu time from
being averaged together with load time.
"""
import argparse
import bisect
import collections
import ctypes as C
import ctypes.wintypes as W
import subprocess
import time

import numpy as np
import pefile

EXE_PATH = r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe'
NTDLL_PATH = r'C:\Windows\System32\ntdll.dll'

# Samples landing in these ntdll exports mean a thread is WAITING, not working.
WAIT_EXPORTS = {
    'RtlSleepConditionVariableSRW', 'RtlSleepConditionVariableCS',
    'ZwDelayExecution', 'RtlDelayExecution', 'ZwWaitForAlertByThreadId',
    'ZwWaitForSingleObject', 'ZwWaitForMultipleObjects', 'ZwRemoveIoCompletion',
    'ZwRemoveIoCompletionEx', 'RtlAcquireSRWLockExclusive', 'RtlAcquireSRWLockShared',
    'RtlEnterCriticalSection', 'ZwWaitForWorkViaWorkerFactory',
    # A blocking syscall, not work. Under Sandboxie this DOMINATES: Sbie's driver
    # RPC goes through DeviceIoControl, so a boxed instance parks ~94% of its
    # threads here even sitting idle at the title menu. Counting it as work made
    # boxed instances read as "100% working" while doing nothing.
    'ZwDeviceIoControlFile',
}

k32 = C.WinDLL('kernel32', use_last_error=True)
THREAD_SUSPEND_RESUME, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION = 0x0002, 0x0008, 0x0040
CONTEXT_CONTROL = 0x00100000 | 0x1
CONTEXT_SIZE, RIP_OFF, FLAGS_OFF = 1232, 0xF8, 0x30
TH32CS_SNAPMODULE, TH32CS_SNAPTHREAD = 0x18, 0x00000004

k32.OpenThread.restype = W.HANDLE
k32.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.GetThreadContext.argtypes = [W.HANDLE, C.c_void_p]

NL = chr(10)


class MODULEENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("a", W.DWORD), ("b", W.DWORD), ("c", W.DWORD),
                ("d", W.DWORD), ("modBaseAddr", C.POINTER(C.c_byte)),
                ("modBaseSize", W.DWORD), ("hModule", W.HMODULE),
                ("szModule", C.c_char * 256), ("szExePath", C.c_char * 260)]


class THREADENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                ("th32OwnerProcessID", W.DWORD), ("tpBasePri", C.c_long),
                ("tpDeltaPri", C.c_long), ("dwFlags", W.DWORD)]


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    me = MODULEENTRY32()
    me.dwSize = C.sizeof(MODULEENTRY32)
    out = []
    if k32.Module32First(snap, C.byref(me)):
        while True:
            b = C.cast(me.modBaseAddr, C.c_void_p).value
            out.append((b, b + me.modBaseSize, me.szModule.decode(errors='replace')))
            if not k32.Module32Next(snap, C.byref(me)):
                break
    k32.CloseHandle(snap)
    return out


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = C.sizeof(THREADENTRY32)
    out = []
    if k32.Thread32First(snap, C.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                out.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, C.byref(te)):
                break
    k32.CloseHandle(snap)
    return out


def pdata(path):
    """(imagebase, function start/end RVA table) from a PE's .pdata."""
    pe = pefile.PE(path, fast_load=True)
    raw = open(path, 'rb').read()
    sec = [s for s in pe.sections if s.Name.rstrip(b'\x00') == b'.pdata']
    if not sec:
        return pe.OPTIONAL_HEADER.ImageBase, None
    s = sec[0]
    n = (s.SizeOfRawData // 12) * 12      # entries are 12 bytes; trailing slack breaks numpy
    arr = np.frombuffer(raw[s.PointerToRawData:s.PointerToRawData + n],
                        dtype=np.dtype([('b', '<u4'), ('e', '<u4'), ('u', '<u4')]))
    return pe.OPTIONAL_HEADER.ImageBase, arr[arr['b'] > 0]


def ntdll_exports():
    pe = pefile.PE(NTDLL_PATH)
    pe.parse_data_directories()
    return sorted((e.address, (e.name or b'').decode(errors='replace'))
                  for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.address)


class ProcState:
    """Per-process sampling state.

    Kept separate per pid so the host and the joiners can be compared: they run
    identical code on an identical map, so anything that differs between their
    profiles is a real asymmetry rather than a property of the workload.
    """

    def __init__(self, pid, exe_ib, exe_fn, exports, exp_addrs):
        self.pid = pid
        self.mods = modules(pid)
        self.exe = next((m for m in self.mods if 'TransportFever2' in m[2]), None)
        self.ntd = next((m for m in self.mods if m[2].lower() == 'ntdll.dll'), None)
        self.exe_ib, self.exe_fn = exe_ib, exe_fn
        self.exports, self.exp_addrs = exports, exp_addrs
        self.handles = {}
        self.hist, self.waits = collections.Counter(), collections.Counter()
        self.win, self.winw = collections.Counter(), collections.Counter()
        # Per-thread: how many working/waiting samples each tid contributed, and
        # what each tid was doing. The busiest thread's OWN histogram is the
        # critical path; the global one is an average over ~140 threads and
        # flatters whatever many threads touch briefly.
        self.tid_work, self.tid_wait = collections.Counter(), collections.Counter()
        self.by_tid = collections.defaultdict(collections.Counter)
        self.win_tid = collections.Counter()
        self.win_by_tid = collections.defaultdict(collections.Counter)
        self.interval = 0.02      # overwritten from argv so CPU-s are honest

    def refresh(self):
        for tid in threads(self.pid):
            if tid not in self.handles:
                h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, False, tid)
                if h:
                    self.handles[tid] = h

    def classify(self, rip):
        """(key, is_wait) for one instruction pointer."""
        if self.ntd and self.ntd[0] <= rip < self.ntd[1]:
            i = bisect.bisect_right(self.exp_addrs, rip - self.ntd[0]) - 1
            if i < 0:
                return 'ntdll!?', False
            name = self.exports[i][1]
            off = (rip - self.ntd[0]) - self.exports[i][0]
            # NOTE: nearest PRECEDING export. Internal (unexported) ntdll functions
            # get labelled with whatever export sits below them, so a large +0x...
            # offset means the label is meaningless and only the address is real.
            return 'ntdll!%s+0x%x' % (name, off), name in WAIT_EXPORTS
        if self.exe and self.exe[0] <= rip < self.exe[1] and self.exe_fn is not None:
            rva = rip - self.exe[0]
            m = self.exe_fn[(self.exe_fn['b'] <= rva) & (rva < self.exe_fn['e'])]
            if len(m):
                return 'exe!0x%x' % (self.exe_ib + int(m[0]['b'])), False
            return 'exe!<no pdata>', False
        return next((n for lo, hi, n in self.mods if lo <= rip < hi), '<unmapped>'), False

    def sample(self, ctx):
        for tid, h in list(self.handles.items()):
            C.memset(ctx, 0, CONTEXT_SIZE)
            C.c_uint32.from_address(ctx + FLAGS_OFF).value = CONTEXT_CONTROL
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                k32.CloseHandle(h)
                del self.handles[tid]
                continue
            ok = k32.GetThreadContext(h, C.c_void_p(ctx))
            k32.ResumeThread(h)
            if not ok:
                continue
            key, is_wait = self.classify(C.c_uint64.from_address(ctx + RIP_OFF).value)
            if is_wait:
                self.waits[key] += 1
                self.winw[key] += 1
                self.tid_wait[tid] += 1
            else:
                self.hist[key] += 1
                self.win[key] += 1
                # PER-THREAD attribution. Without this the global histogram cannot
                # tell "one thread spent 36% of the load here" from "forty threads
                # each touched it briefly" -- and on a 32-core box with 64 pool
                # workers parked in _Cnd_wait, that difference is the whole
                # analysis. Every conclusion about the critical path is gated on
                # knowing WHICH thread a sample came from.
                self.tid_work[tid] += 1
                self.by_tid[tid][key] += 1
                self.win_tid[tid] += 1
                self.win_by_tid[tid][key] += 1

    def dump_window(self, top=8):
        b = sum(self.win.values())
        i = sum(self.winw.values())
        print('  --- pid %d: %d working / %d waiting (%.1f%% working) ---'
              % (self.pid, b, i, 100.0 * b / max(b + i, 1)), flush=True)
        for k, c in self.win.most_common(top):
            print('      %6d %5.1f%%  %s' % (c, 100.0 * c / max(b, 1), k), flush=True)
        # PER-THREAD, PER WINDOW. Printing this only in the final totals was
        # wrong twice over: the totals dilute the load with whatever happens
        # afterwards, and ranking by CUMULATIVE samples always crowns the steady
        # repaint thread rather than the one doing the load. Window-local counts
        # answer "which thread is on the critical path RIGHT NOW".
        if self.win_tid:
            print('      -- busiest threads THIS window --', flush=True)
            for tid, n in self.win_tid.most_common(3):
                own = self.win_by_tid[tid].most_common(3)
                detail = ', '.join('%s %.0f%%' % (k, 100.0 * c / max(n, 1)) for k, c in own)
                print('      tid %-7d %5d working (%5.1f CPU-s)  %s'
                      % (tid, n, n * self.interval, detail), flush=True)
        self.win.clear()
        self.winw.clear()
        self.win_tid.clear()
        self.win_by_tid.clear()

    def dump_total(self, top=20, interval=0.02, per_thread=3):
        b = sum(self.hist.values())
        i = sum(self.waits.values())
        print(NL + '=== pid %d: %d working / %d waiting (%.1f%% working) ==='
              % (self.pid, b, i, 100.0 * b / max(b + i, 1)))
        # Samples -> CPU-seconds. One sample = one thread observed for one
        # interval, so the pool-wide total can exceed wall clock; divide by wall
        # clock to get "cores busy". Reporting only percentages invites the
        # mistake of multiplying a share by wall time, which over-counts by
        # exactly the concurrency factor.
        print('   (1 sample = %.0f ms of one thread; %d working samples = %.0f CPU-s)'
              % (interval * 1000, b, b * interval))
        for k, c in self.hist.most_common(top):
            print('   %7d %5.1f%%  %8.1f CPU-s  %s'
                  % (c, 100.0 * c / max(b, 1), c * interval, k))
        print('   -- top waits (idle threads, NOT time spent) --')
        for k, c in self.waits.most_common(4):
            print('   %7d %5.1f%%  %s' % (c, 100.0 * c / max(i, 1), k))

        # THE CRITICAL PATH. A long load is usually one thread's serial work; the
        # global table above averages it away across every idle pool worker.
        print(NL + '   --- busiest threads by WORKING samples ---')
        for tid, c in self.tid_work.most_common(8):
            print('   tid %-7d %7d working (%6.1f CPU-s)  %6d waiting'
                  % (tid, c, c * interval, self.tid_wait[tid]))
        for tid, c in self.tid_work.most_common(per_thread):
            own = self.by_tid[tid]
            tot = sum(own.values())
            print(NL + '   === tid %d in detail: %d working samples = %.0f CPU-s ==='
                  % (tid, tot, tot * interval))
            for k, n in own.most_common(10):
                print('      %7d %5.1f%%  %8.1f CPU-s  %s'
                      % (n, 100.0 * n / max(tot, 1), n * interval, k))


def game_pids():
    out = subprocess.run(['powershell', '-NoProfile', '-Command',
                          '(Get-Process TransportFever2 -EA SilentlyContinue).Id'],
                         capture_output=True, text=True)
    return [int(x) for x in out.stdout.split() if x.strip().isdigit()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--wait-for-game', action='store_true')
    ap.add_argument('--all', action='store_true',
                    help='profile EVERY TransportFever2 process, reported per pid')
    ap.add_argument('--seconds', type=float, default=120.0)
    ap.add_argument('--interval', type=float, default=0.02)
    ap.add_argument('--window', type=float, default=30.0, help='seconds per delta snapshot')
    a = ap.parse_args()

    exe_ib, exe_fn = pdata(EXE_PATH)
    exports = ntdll_exports()
    exp_addrs = [x for x, _ in exports]

    # Resolve the target set, waiting for the game if asked, so this can be armed
    # before a relaunch and still catch the load from its first second.
    pids = []
    deadline = time.time() + 600
    while time.time() < deadline:
        if a.pid:
            pids = [a.pid]
        else:
            found = game_pids()
            pids = found if a.all else found[:1]
        if pids:
            break
        time.sleep(1.0)
    if not pids:
        print('no TransportFever2 process appeared')
        return

    # Sampling N processes costs a suspend/resume pair per thread per tick. Three
    # instances is ~420 threads, so relax the interval rather than let the
    # profiler perturb the thing it is measuring.
    if len(pids) > 1 and a.interval < 0.05:
        a.interval = 0.05
    print('profiling pids %s for %.0fs at %.0f ms (window %.0fs)'
          % (pids, a.seconds, a.interval * 1000, a.window), flush=True)

    states = [ProcState(p, exe_ib, exe_fn, exports, exp_addrs) for p in pids]
    for st in states:
        st.interval = a.interval
    raw_ctx = (C.c_char * (CONTEXT_SIZE + 16))()
    ctx = (C.addressof(raw_ctx) + 15) & ~15

    t0 = win_start = time.time()
    end = t0 + a.seconds
    last_refresh = 0.0
    while time.time() < end:
        if time.time() - last_refresh > 2.0:
            for s in states:
                s.refresh()                   # loads spawn threads part-way through
            last_refresh = time.time()
        for s in states:
            s.sample(ctx)
        if time.time() - win_start >= a.window:
            print(NL + '########## t+%.0fs ##########' % (time.time() - t0), flush=True)
            for s in states:
                s.dump_window()
            win_start = time.time()
        time.sleep(a.interval)

    print(NL + NL + '================ TOTALS ================')
    for st in states:
        st.dump_total(interval=a.interval)


if __name__ == '__main__':
    main()
