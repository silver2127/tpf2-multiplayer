# Generic "find the function behind a string" tool -- the M1 technique.
#   python find_sym.py forEachEntityWithComponent getComponent
# Finds each exact ASCII string, its rip-relative references, and the enclosing
# .pdata function, then prints the other strings that function touches so it can
# be identified.
import sys, bisect
import numpy as np, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
raw = open(EXE, "rb").read()
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
sections = [(s.VirtualAddress, s.PointerToRawData, s.SizeOfRawData,
             s.Name.rstrip(b"\0").decode("ascii", "replace")) for s in pe.sections]

def off_to_va(off):
    for rva, rawptr, rawsize, _ in sections:
        if rawptr <= off < rawptr + rawsize:
            return BASE + rva + (off - rawptr)
    return None

def va_to_off(va):
    r = va - BASE
    for rva, rawptr, rawsize, _ in sections:
        if rva <= r < rva + rawsize:
            return rawptr + (r - rva)
    return None

TEXT_RVA, TEXT_RAW, TEXT_SIZE, _ = [s for s in sections if s[3] == ".text"][0]
TEXT_VA = BASE + TEXT_RVA
code = raw[TEXT_RAW:TEXT_RAW + TEXT_SIZE]

PD_RAW, PD_SIZE = [(s[1], s[2]) for s in sections if s[3] == ".pdata"][0]
pd = np.frombuffer(raw[PD_RAW:PD_RAW + PD_SIZE],
                   dtype=np.dtype([("begin", "<u4"), ("end", "<u4"), ("unwind", "<u4")]))
begins = sorted(int(b) + BASE for b in pd["begin"])
end_of = {int(b) + BASE: int(e) + BASE for b, e in zip(pd["begin"], pd["end"])}

def containing_func(va):
    i = bisect.bisect_right(begins, va) - 1
    if i < 0: return None
    b = begins[i]
    return b if b <= va < end_of.get(b, 0) else None

data8 = np.frombuffer(code, dtype=np.uint8)
w = np.lib.stride_tricks.sliding_window_view(data8, 4)
arr32 = (w[:, 0].astype(np.uint32) | (w[:, 1].astype(np.uint32) << 8)
         | (w[:, 2].astype(np.uint32) << 16) | (w[:, 3].astype(np.uint32) << 24))
disp = arr32.astype(np.int32).astype(np.int64)
pidx = np.arange(len(disp), dtype=np.int64)
END_VA = TEXT_VA + pidx + 4
MODRM = np.zeros(len(disp), dtype=np.uint8)
MODRM[1:] = data8[:len(disp) - 1]

def riprel_refs(target_va):
    match = (END_VA + disp) == target_va
    ok = (MODRM & 0xC7) == 0x05
    return [int(TEXT_VA + p) for p in np.nonzero(match & ok)[0]]

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

def read_ascii(va, maxlen=140):
    o = va_to_off(va)
    if o is None: return None
    e = raw.find(b"\0", o, o + maxlen)
    if e <= o: return None
    try: s = raw[o:e].decode("ascii")
    except UnicodeDecodeError: return None
    return s if len(s) >= 3 and all(32 <= ord(c) < 127 for c in s) else None

def strings_in(f, limit=3000):
    o, e = va_to_off(f), end_of.get(f)
    if o is None or not e: return []
    out = []
    for i, insn in enumerate(md.disasm(raw[o:va_to_off(e)], f)):
        if i > limit: break
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                s = read_ascii(insn.address + insn.size + op.mem.disp)
                if s and s not in out: out.append(s)
    return out

def find_string_vas(needle: bytes):
    vas, start = [], 0
    while True:
        i = raw.find(needle, start)
        if i < 0: break
        if i == 0 or raw[i - 1] == 0:
            va = off_to_va(i)
            if va: vas.append(va)
        start = i + 1
    return vas

for name in sys.argv[1:]:
    print("=" * 74)
    print(f"## {name!r}")
    for va in find_string_vas(name.encode() + b"\0"):
        refs = riprel_refs(va)
        print(f"  string @0x{va:x}: {len(refs)} ref(s)")
        for r in refs[:8]:
            f = containing_func(r)
            if not f:
                print(f"    ref 0x{r:x} -> no .pdata function")
                continue
            ss = [s for s in strings_in(f) if s != name][:8]
            print(f"    ref 0x{r:x} -> func 0x{f:x} [{end_of[f]-f}b]")
            print(f"        nearby: {ss}")
