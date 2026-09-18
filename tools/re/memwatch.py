r"""memwatch.py -- read-only memory timeline of a running TransportFever2.exe.

Samples the process every `--interval` seconds (default 1) with
GetProcessMemoryInfo, and every `--census` samples (default 3) walks the
address space with VirtualQueryEx to bucket committed regions by type and
size. Big private regions (>= --big MB, default 256) are tracked by base
address across censuses, so their lifetime shows up as "+" when they appear
and "-" when they go: a load's transient giants are the ones that appear
during the load and vanish before it ends.

Never opens the process for writing (PROCESS_QUERY_INFORMATION only), never
launches or signals anything. Pick the native instance automatically (the
one without SbieDll.dll), or pass --pid.

    python tools\re\memwatch.py --out C:\path\memwatch.txt
"""
import argparse, ctypes as C, ctypes.wintypes as W, os, sys, time, collections

k32 = C.windll.kernel32
psapi = C.windll.psapi
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE = 0x20000, 0x40000, 0x1000000

class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('PartitionId', W.WORD), ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]

class PMC(C.Structure):
    _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD), ('PeakWorkingSetSize', C.c_size_t), ('WorkingSetSize', C.c_size_t),
                ('QuotaPeakPagedPoolUsage', C.c_size_t), ('QuotaPagedPoolUsage', C.c_size_t), ('QuotaPeakNonPagedPoolUsage', C.c_size_t),
                ('QuotaNonPagedPoolUsage', C.c_size_t), ('PagefileUsage', C.c_size_t), ('PeakPagefileUsage', C.c_size_t), ('PrivateUsage', C.c_size_t)]

class MSX(C.Structure):
    _fields_ = [('dwLength', W.DWORD), ('dwMemoryLoad', W.DWORD), ('ullTotalPhys', C.c_uint64), ('ullAvailPhys', C.c_uint64),
                ('ullTotalPageFile', C.c_uint64), ('ullAvailPageFile', C.c_uint64), ('ullTotalVirtual', C.c_uint64),
                ('ullAvailVirtual', C.c_uint64), ('ullAvailExtendedVirtual', C.c_uint64)]

k32.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(MBI), C.c_size_t]
k32.VirtualQueryEx.restype = C.c_size_t
k32.OpenProcess.restype = W.HANDLE
psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(PMC), W.DWORD]
psapi.EnumProcessModulesEx.argtypes = [W.HANDLE, C.POINTER(W.HMODULE), W.DWORD, C.POINTER(W.DWORD), W.DWORD]
psapi.GetModuleBaseNameW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]

def pids_named(name):
    arr = (W.DWORD * 4096)(); n = W.DWORD()
    psapi.EnumProcesses(arr, C.sizeof(arr), C.byref(n))
    out = []
    for pid in arr[:n.value // 4]:
        h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not h: continue
        buf = C.create_unicode_buffer(260)
        mod = W.HMODULE(); need = W.DWORD()
        if psapi.EnumProcessModulesEx(h, C.byref(mod), C.sizeof(mod), C.byref(need), 0x03) and psapi.GetModuleBaseNameW(h, mod, buf, 260):
            if buf.value.lower() == name.lower():
                out.append((pid, h)); continue
        k32.CloseHandle(h)
    return out

def has_module(h, name):
    mods = (W.HMODULE * 1024)(); need = W.DWORD()
    if not psapi.EnumProcessModulesEx(h, mods, C.sizeof(mods), C.byref(need), 0x03): return False
    buf = C.create_unicode_buffer(260)
    for m in mods[:min(1024, need.value // C.sizeof(W.HMODULE))]:
        if psapi.GetModuleBaseNameW(h, m, buf, 260) and buf.value.lower() == name.lower(): return True
    return False

def census(h, big):
    addr = 0; mbi = MBI()
    by = collections.Counter(); bigs = {}
    while k32.VirtualQueryEx(h, addr, C.byref(mbi), C.sizeof(mbi)):
        if mbi.State == MEM_COMMIT:
            t = 'private' if mbi.Type == MEM_PRIVATE else 'mapped' if mbi.Type == MEM_MAPPED else 'image'
            by[t] += mbi.RegionSize
            if mbi.Type == MEM_PRIVATE:
                cls = '<1M' if mbi.RegionSize < 1 << 20 else '<16M' if mbi.RegionSize < 16 << 20 else '<256M' if mbi.RegionSize < 256 << 20 else '>=256M'
                by['private ' + cls] += mbi.RegionSize
            if mbi.RegionSize >= big and mbi.Type != MEM_IMAGE:
                bigs[mbi.AllocationBase] = bigs.get(mbi.AllocationBase, 0) + mbi.RegionSize
        nxt = (mbi.BaseAddress or 0) + mbi.RegionSize
        if nxt <= addr: break
        addr = nxt
    return by, bigs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int); ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--census', type=int, default=3); ap.add_argument('--big', type=int, default=256, help='MB')
    ap.add_argument('--out'); ap.add_argument('--minutes', type=float, default=30)
    a = ap.parse_args()
    out = open(a.out, 'a', buffering=1) if a.out else sys.stdout
    def say(s):
        out.write(time.strftime('%H:%M:%S ') + s + '\n'); out.flush()
    # Wait for the game (the user launches it); prefer the native instance.
    t0 = time.time(); said = False
    while True:
        procs = pids_named('TransportFever2.exe')
        if a.pid: procs = [p for p in procs if p[0] == a.pid]
        else: procs = [p for p in procs if not has_module(p[1], 'SbieDll.dll')] or procs
        if procs: break
        if not said: say('waiting for TransportFever2.exe'); said = True
        if time.time() - t0 > a.minutes * 60: say('no game within the time limit'); return 1
        time.sleep(2)
    pid, h = procs[0]
    say(f'watching pid {pid} (sandboxed={has_module(h, "SbieDll.dll")}) every {a.interval}s, census every {a.census} samples, big >= {a.big} MB')
    known = {}; n = 0; t0 = time.time(); last_line = ''
    GB = 1 << 30
    while time.time() - t0 < a.minutes * 60:
        pmc = PMC(); pmc.cb = C.sizeof(pmc)
        if not psapi.GetProcessMemoryInfo(h, C.byref(pmc), C.sizeof(pmc)):
            say('process gone'); break
        ms = MSX(); ms.dwLength = C.sizeof(ms); k32.GlobalMemoryStatusEx(C.byref(ms))
        line = f'private={pmc.PrivateUsage / GB:.2f}G ws={pmc.WorkingSetSize / GB:.2f}G peakws={pmc.PeakWorkingSetSize / GB:.2f}G faults={pmc.PageFaultCount} sys_commit_free={ms.ullAvailPageFile / GB:.1f}G phys_free={ms.ullAvailPhys / GB:.1f}G'
        if n % a.census == 0:
            by, bigs = census(h, a.big << 20)
            line += ' | committed private=%.2fG mapped=%.2fG [%s]' % (by['private'] / GB, by['mapped'] / GB,
                ' '.join(f'{k.split()[1]}={v / GB:.2f}G' for k, v in sorted(by.items()) if k.startswith('private ') ))
            for base, size in bigs.items():
                if base not in known: say(f'  + region 0x{base:x} {size / (1 << 20):.0f} MB')
            for base in list(known):
                if base not in bigs: say(f'  - region 0x{base:x} {known[base] / (1 << 20):.0f} MB gone')
            for base, size in bigs.items():
                if base in known and abs(size - known[base]) >= (64 << 20): say(f'  ~ region 0x{base:x} {known[base] / (1 << 20):.0f} -> {size / (1 << 20):.0f} MB')
            known = bigs
        if line != last_line: say(line); last_line = line
        n += 1; time.sleep(a.interval)
    return 0

if __name__ == '__main__': sys.exit(main())
