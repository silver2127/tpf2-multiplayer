# Locate the title-screen menu builder (UI::CMenuUI::CreatePageMain) so the
# bridge can hook it and append a Multiplayer entry.
#
# Method is the same one M1 used: the binary keeps ASCII profiling/assert
# strings next to the code that uses them, so find the string, find the
# rip-relative references to it, and use .pdata to get the enclosing function.
# Then print the other strings each candidate touches -- that is effectively
# the function's name tag.
import os, bisect, sys
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "menu_ui_map.txt")

raw = open(EXE, "rb").read()
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
sections = []
for s in pe.sections:
    sections.append((s.VirtualAddress, s.PointerToRawData, s.SizeOfRawData,
                     s.Name.rstrip(b"\0").decode("ascii", "replace")))

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

PD_RVA, PD_RAW, PD_SIZE, _ = [s for s in sections if s[3] == ".pdata"][0]
pd = np.frombuffer(raw[PD_RAW:PD_RAW + PD_SIZE],
                   dtype=np.dtype([("begin", "<u4"), ("end", "<u4"), ("unwind", "<u4")]))
begins = sorted(int(b) + BASE for b in pd["begin"])
end_of = {int(b) + BASE: int(e) + BASE for b, e in zip(pd["begin"], pd["end"])}

def containing_func(va):
    i = bisect.bisect_right(begins, va) - 1
    if i < 0:
        return None
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

def call_refs(func_va):
    opb = np.zeros(len(disp), dtype=np.uint8)
    opb[1:] = data8[:len(disp) - 1]
    target = TEXT_VA + pidx + 4 + disp
    res = []
    for opcode in (0xE8, 0xE9):
        for p in np.nonzero((target == func_va) & (opb == opcode))[0]:
            res.append(int(TEXT_VA + p - 1))
    return res

def read_ascii(va, maxlen=200):
    off = va_to_off(va)
    if off is None:
        return None
    end = raw.find(b"\0", off, off + maxlen)
    if end <= off:
        return None
    try:
        s = raw[off:end].decode("ascii")
    except UnicodeDecodeError:
        return None
    return s if len(s) >= 3 and all(32 <= ord(c) < 127 for c in s) else None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def strings_in(func_va, limit=4000):
    """every ASCII string a function references -- its de-facto identity"""
    off = va_to_off(func_va)
    end = end_of.get(func_va)
    if off is None or not end:
        return []
    blob = raw[off:va_to_off(end)]
    found = []
    for i, insn in enumerate(md.disasm(blob, func_va)):
        if i > limit:
            break
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                s = read_ascii(insn.address + insn.size + op.mem.disp)
                if s and s not in found:
                    found.append(s)
    return found

def find_string_vas(needle: bytes):
    vas, start = [], 0
    while True:
        i = raw.find(needle, start)
        if i < 0:
            break
        # must be a standalone string: preceded by NUL
        if i == 0 or raw[i - 1] == 0:
            va = off_to_va(i)
            if va:
                vas.append(va)
        start = i + 1
    return vas

out = open(OUT, "w", encoding="utf-8")
def emit(s=""):
    print(s)
    out.write(s + "\n")

TARGETS = [
    b"MainMenu\0",
    b"UI::CMenuUI::CreatePageMain",
    b"void __cdecl UI::CMenuUI::CreatePage(enum UI::CMenuUI::Page)\0",
]

candidates = {}
for needle in TARGETS:
    label = needle.rstrip(b"\0").decode("ascii", "replace")
    emit("=" * 78)
    emit(f"## string: {label!r}")
    vas = find_string_vas(needle)
    if not vas:
        emit("   (not found)")
        continue
    for va in vas:
        refs = riprel_refs(va)
        emit(f"   at 0x{va:x}: {len(refs)} rip-rel reference(s)")
        for r in refs:
            f = containing_func(r)
            if not f:
                emit(f"     ref 0x{r:x} -> no .pdata function")
                continue
            emit(f"     ref 0x{r:x} -> func 0x{f:x} "
                 f"[0x{f:x}-0x{end_of.get(f,0):x}, {end_of.get(f,0)-f} bytes]")
            candidates.setdefault(f, set()).add(label)

emit()
emit("=" * 78)
emit("## candidate functions, identified by the strings they reference")
for f in sorted(candidates):
    emit("")
    emit(f"### func 0x{f:x}  (matched: {', '.join(sorted(candidates[f]))})")
    emit(f"    size {end_of.get(f,0)-f} bytes, {len(call_refs(f))} direct caller(s)")
    ss = strings_in(f)
    for s in ss[:40]:
        emit(f"      str: {s!r}")
    if len(ss) > 40:
        emit(f"      ... {len(ss)-40} more")

out.close()
print("\nwrote", OUT)
