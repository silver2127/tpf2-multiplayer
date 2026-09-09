# M1 phase 3 (working): RIP-relative xref engine + .pdata-exact functions.
#
# Validated: strings are referenced by LEA reg,[rip+disp32] in .text.
# .pdata RUNTIME_FUNCTION table (138k entries) gives exact function bounds.
#
# Produces:
#   - xref sites + exact containing function for each target string
#   - full annotated disassembly of the api.cmd registration function
#     (every LEA -> resolved string, every CALL -> target function)
#   - direct callers (E8/E9) of each found function, 1 level up
import os, bisect
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_OP_IMM

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
OUT = os.path.join(HERE, "M1_map.txt")

raw = open(EXE, "rb").read()
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase

sec = {}
for s in pe.sections:
    name = s.Name.rstrip(b"\0").decode("ascii", "replace")
    sec[name] = (s.VirtualAddress, s.PointerToRawData, s.SizeOfRawData)

def off_to_va(off):
    for rva, rawptr, rawsize in sec.values():
        if rawptr <= off < rawptr + rawsize:
            return BASE + rva + (off - rawptr)
    return None

def va_to_off(va):
    rva = va - BASE
    for rva, rawptr, rawsize in sec.values():
        if rva <= rva < rva + rawsize:
            return rawptr + (rva - rva) + (rva and (rva - rva))  # placeholder, fixed below
    return None

# fix va_to_off (kept simple):
def va_to_off(va):
    r = va - BASE
    for rva, rawptr, rawsize in sec.values():
        if rva <= r < rva + rawsize:
            return rawptr + (r - rva)
    return None

TEXT_RVA, TEXT_RAW, TEXT_SIZE = sec[".text"]
TEXT_VA = BASE + TEXT_RVA
code = raw[TEXT_RAW:TEXT_RAW + TEXT_SIZE]

# ---- .pdata exact function table --------------------------------------------
PD_RVA, PD_RAW, PD_SIZE = sec[".pdata"]
pd = np.frombuffer(raw[PD_RAW:PD_RAW + PD_SIZE],
                   dtype=np.dtype([("begin", "<u4"), ("end", "<u4"), ("unwind", "<u4")]))
begins = sorted(int(b) + BASE for b in pd["begin"])
end_of = {int(b) + BASE: int(e) + BASE for b, e in zip(pd["begin"], pd["end"])}
print(f"functions: {len(begins):,}")

def containing_func(va):
    i = bisect.bisect_right(begins, va) - 1
    if i < 0:
        return None
    b = begins[i]
    return b if b <= va < end_of.get(b, 0) else None

# ---- vectorized rip-rel scan over .text --------------------------------------
data8 = np.frombuffer(code, dtype=np.uint8)
w = np.lib.stride_tricks.sliding_window_view(data8, 4)
arr32 = (w[:, 0].astype(np.uint32) | (w[:, 1].astype(np.uint32) << 8)
         | (w[:, 2].astype(np.uint32) << 16) | (w[:, 3].astype(np.uint32) << 24))
disp = arr32.astype(np.int32).astype(np.int64)
pidx = np.arange(len(disp), dtype=np.int64)
END_VA = TEXT_VA + pidx + 4          # VA right after a disp32 field at index p
MODRM = np.zeros(len(disp), dtype=np.uint8)
MODRM[1:] = data8[:len(disp) - 1]    # MODRM[p] = byte at p-1
PREV2 = np.zeros(len(disp), dtype=np.uint8)
PREV2[2:] = data8[:len(disp) - 2]    # byte at p-2

def riprel_xrefs(target_va):
    """candidate insn addresses referencing target_va via rip-rel operand"""
    match = (END_VA + disp) == target_va
    ok = (MODRM & 0xC7) == 0x05       # mod=00, rm=101 -> rip-relative
    return [TEXT_VA + p - 2 for p in np.nonzero(match & ok)[0]]  # approx insn start (rex+opcode)

def call_xrefs(func_va):
    """E8/E9 direct call sites targeting func_va"""
    opb = np.zeros(len(disp), dtype=np.uint8)
    opb[1:] = data8[:len(disp) - 1]   # opcode byte at p-1 when disp starts at p
    target = TEXT_VA + pidx + 4 + disp
    res = []
    for opcode in (0xE8, 0xE9):
        for p in np.nonzero((target == func_va) & (opb == opcode))[0]:
            res.append(TEXT_VA + p - 1)
    return res

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def read_ascii(va, maxlen=80):
    off = va_to_off(va)
    if off is None:
        return None
    end = raw.find(b"\0", off, off + maxlen)
    if end <= off:
        return None
    try:
        s = raw[off:end].decode("ascii")
        return s if len(s) >= 2 and all(32 <= ord(c) < 127 for c in s) else None
    except UnicodeDecodeError:
        return None

def find_string(needle):
    nb = needle.encode()
    hits, i = [], 0
    while True:
        i = raw.find(nb, i)
        if i < 0:
            break
        hits.append(i)
        i += 1
    return hits

def disasm_annotated(func_va, max_insns=4000, out=None, headline=None):
    """full function disasm; annotate LEAs with strings, CALLs with func addrs"""
    off = va_to_off(func_va)
    end = end_of.get(func_va, func_va + 0x200)
    blob = raw[off:va_to_off(end)]
    lines = []
    n = 0
    for insn in md.disasm(blob, func_va):
        note = ""
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                tgt = insn.address + insn.size + op.mem.disp
                s = read_ascii(tgt)
                note = f"; -> 0x{tgt:x}" + (f" {s!r}" if s else "")
            elif op.type == X86_OP_IMM and insn.mnemonic.startswith("call"):
                f = containing_func(op.imm)
                note = f"; -> func 0x{op.imm:x}" + ("" if f == op.imm else (f" (in 0x{f:x})" if f else " [not in .pdata]"))
        lines.append(f"  0x{insn.address:x}  {insn.mnemonic:8s} {insn.op_str}{note}")
        n += 1
        if n >= max_insns:
            lines.append("  ...(truncated)")
            break
    if out:
        if headline:
            out.write(headline + "\n")
        out.write("\n".join(lines) + "\n")
    return lines

out = open(OUT, "w", encoding="utf-8")

def analyze(label, needle, max_hits=4, disasm=False):
    out.write(f"\n{'='*72}\n## {label}: {needle!r}\n")
    print(f"\n== {label}")
    funcs = []
    for so in find_string(needle)[:max_hits]:
        # back up to the start of the NUL-terminated string; LEA targets the start
        so = raw.rfind(b"\0", 0, so) + 1
        va = off_to_va(so)
        xs = riprel_xrefs(va)
        out.write(f"string @0x{so:x} VA 0x{va:x}: {len(xs)} xref(s)\n")
        print(f"  VA 0x{va:x}: {len(xs)} xref(s)")
        for x in xs:
            f = containing_func(x)
            out.write(f"  xref ~0x{x:x}  in func 0x{f:x} [0x{f:x}-0x{end_of.get(f,0):x}]\n" if f else f"  xref ~0x{x:x}  (no func?!)\n")
            print(f"    xref 0x{x:x} -> func 0x{f:x}" if f else f"    xref 0x{x:x} -> ?")
            if f:
                funcs.append(f)
                if disasm:
                    disasm_annotated(f, out=out, headline=f"  -- disasm of 0x{f:x} --")
    return funcs

targets = [
    ("api.cmd sendCommand", "sendCommand", True),
    ("api.cmd SetupCommandInterface", "SetupCommandInterface", False),
    ("CommandList.cpp assert", r"\command\CommandList.cpp", True),
    ("CommandList::Swap sig", "CommandList::Swap", False),
    ("apply_command.cpp assert", r"\command\apply_command.cpp", False),
    ("make_command.cpp assert", r"\command\make_command.cpp", False),
    ("GameSim.cpp assert", r"\Game\GameSim.cpp", False),
    ("GameTime.cpp assert", r"\Game\GameTime.cpp", False),
    ("GameState.cpp assert", r"\Game\GameState.cpp", False),
    ("Serializer.cpp assert", r"\Game\Serializer.cpp", False),
]

found = {}
for label, needle, dz in targets:
    found[label] = analyze(label, needle, disasm=dz)

# callers, one level up
out.write(f"\n\n{'='*72}\n## CALLERS (1 level)\n")
seen = set()
for label, funcs in found.items():
    for f in sorted(set(funcs)):
        if f in seen:
            continue
        seen.add(f)
        cs = call_xrefs(f)
        out.write(f"\n[{label}] func 0x{f:x}: {len(cs)} caller(s)\n")
        print(f"[{label}] 0x{f:x}: {len(cs)} callers")
        for c in cs[:20]:
            cf = containing_func(c)
            out.write(f"    called from 0x{c:x} (in func 0x{cf:x})\n" if cf else f"    called from 0x{c:x}\n")

out.close()
print("\nwrote", OUT)
