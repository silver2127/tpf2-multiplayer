# M1 phase 4: targeted follow-up queries.
#  1. identify + disassemble the function containing the call to the
#     command-index executor (the per-tick apply loop candidate)
#  2. address-taken scan: find LEA references to CommandList::Swap and to
#     the apply loop (indirect-call registration sites)
#  3. walk one level up from those sites to the tick/heartbeat function
import os, bisect
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_OP_IMM

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
OUT = os.path.join(HERE, "M1_walk.txt")

raw = open(EXE, "rb").read()
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
sec = {}
for s in pe.sections:
    name = s.Name.rstrip(b"\0").decode("ascii", "replace")
    sec[name] = (s.VirtualAddress, s.PointerToRawData, s.SizeOfRawData)

def va_to_off(va):
    r = va - BASE
    for rva, rawptr, rawsize in sec.values():
        if rva <= r < rva + rawsize:
            return rawptr + (r - rva)
    return None

TEXT_RVA, TEXT_RAW, TEXT_SIZE = sec[".text"]
TEXT_VA = BASE + TEXT_RVA
code = raw[TEXT_RAW:TEXT_RAW + TEXT_SIZE]

PD_RVA, PD_RAW, PD_SIZE = sec[".pdata"]
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
    """any rip-rel reference (lea/mov/...) to target_va; returns insn-ish addrs"""
    match = (END_VA + disp) == target_va
    ok = (MODRM & 0xC7) == 0x05
    return [TEXT_VA + p for p in np.nonzero(match & ok)[0]]

def call_refs(func_va):
    opb = np.zeros(len(disp), dtype=np.uint8)
    opb[1:] = data8[:len(disp) - 1]
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

def disasm_annotated(func_va, max_insns=1200):
    off = va_to_off(func_va)
    end = end_of.get(func_va, func_va + 0x200)
    blob = raw[off:va_to_off(end)]
    lines = []
    for i, insn in enumerate(md.disasm(blob, func_va)):
        note = ""
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                tgt = insn.address + insn.size + op.mem.disp
                s = read_ascii(tgt)
                f = containing_func(tgt)
                extra = f" [string {s!r}]" if s else (f" [func 0x{f:x}]" if f == tgt else "")
                note = f"; -> 0x{tgt:x}{extra}"
            elif op.type == X86_OP_IMM and insn.mnemonic.startswith(("call", "j")):
                f = containing_func(op.imm)
                note = f"; -> 0x{op.imm:x}" + ("" if f == op.imm else (f" (in 0x{f:x})" if f else " [not in .pdata]"))
        lines.append(f"  0x{insn.address:x}  {insn.mnemonic:8s} {insn.op_str}{note}")
        if i >= max_insns:
            lines.append("  ...(truncated)")
            break
    return lines

out = open(OUT, "w", encoding="utf-8")

def show(label, va, disasm=True):
    f = containing_func(va)
    out.write(f"\n{'='*72}\n## {label}: 0x{va:x} -> func {hex(f) if f else '?'}"
              f" [0x{f:x}-0x{end_of.get(f,0):x}]\n" if f else f"\n## {label}: 0x{va:x} no func\n")
    print(f"{label}: 0x{va:x} -> func {hex(f) if f else '?'}")
    if f and disasm:
        out.write("\n".join(disasm_annotated(f)) + "\n")
    return f

# 1. the apply loop: function containing the call to command-index executor
apply_loop = show("apply-loop (caller of cmd executor)", 0x1409d34d3)

# 2. who calls the apply loop?
if apply_loop:
    out.write(f"\ncallers of apply loop 0x{apply_loop:x}:\n")
    for c in call_refs(apply_loop):
        cf = containing_func(c)
        out.write(f"    call from 0x{c:x} (func {hex(cf) if cf else '?'})\n")
        print(f"  apply-loop caller: 0x{c:x} func {hex(cf) if cf else '?'}")
        if cf:
            show("apply-loop caller func", cf)
    # address-taken refs to apply loop
    refs = riprel_refs(apply_loop)
    out.write(f"\nrip-rel refs to 0x{apply_loop:x} (address-taken/lea): {len(refs)}\n")
    for r in refs[:20]:
        rf = containing_func(r - 4)
        out.write(f"    ref at ~0x{r:x} (func {hex(rf) if rf else '?'})\n")
        print(f"  addr-taken ref ~0x{r:x} func {hex(rf) if rf else '?'}")

# 3. address-taken refs to CommandList::Swap
SWAP = 0x1409d2d5f
refs = riprel_refs(SWAP)
out.write(f"\nrip-rel refs to CommandList::Swap 0x{SWAP:x}: {len(refs)}\n")
print(f"Swap refs: {len(refs)}")
for r in refs[:20]:
    rf = containing_func(r - 4)
    out.write(f"    ref at ~0x{r:x} (func {hex(rf) if rf else '?'})\n")
    print(f"  swap ref ~0x{r:x} func {hex(rf) if rf else '?'}")
    if rf:
        show("func taking address of Swap", rf)

# 4. GameSim update chain: caller of the big sim function
show("GameSim update caller (frame/tick candidate)", 0x14014f8b0)
for c in call_refs(0x14014f8b0):
    cf = containing_func(c)
    out.write(f"    GameSim-update called from 0x{c:x} (func {hex(cf) if cf else '?'})\n")
    print(f"  gamesim-update caller 0x{c:x} func {hex(cf) if cf else '?'}")

out.close()
print("\nwrote", OUT)
