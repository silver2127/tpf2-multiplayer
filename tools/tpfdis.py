# Annotated disassembler for TransportFever2.exe.
#   python dis.py 0x1407c5d30            -- whole function containing that VA
#   python dis.py 0x140667cfd -w 60      -- +/- a window of instructions
#   python dis.py 0x140667bc0 -s 0x100 -n 120
# Annotates rip-relative operands with the ASCII string / function they hit,
# and call targets with the function they land in.
import argparse, bisect
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_OP_IMM

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"

raw = open(EXE, "rb").read()
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
sections = [(s.VirtualAddress, s.PointerToRawData, s.SizeOfRawData,
             s.Name.rstrip(b"\0").decode("ascii", "replace")) for s in pe.sections]

def va_to_off(va):
    r = va - BASE
    for rva, rawptr, rawsize, _ in sections:
        if rva <= r < rva + rawsize:
            return rawptr + (r - rva)
    return None

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

def read_ascii(va, maxlen=160):
    off = va_to_off(va)
    if off is None: return None
    end = raw.find(b"\0", off, off + maxlen)
    if end <= off: return None
    try: s = raw[off:end].decode("ascii")
    except UnicodeDecodeError: return None
    return s if len(s) >= 2 and all(32 <= ord(c) < 127 for c in s) else None

def read_qword(va):
    off = va_to_off(va)
    if off is None or off + 8 > len(raw): return None
    return int.from_bytes(raw[off:off+8], "little")

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def annotate(insn):
    notes = []
    for op in insn.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            tgt = insn.address + insn.size + op.mem.disp
            s = read_ascii(tgt)
            if s:
                notes.append(f'"{s}"')
                continue
            f = containing_func(tgt)
            if f == tgt:
                notes.append(f"func 0x{tgt:x}")
                continue
            q = read_qword(tgt)
            if q and containing_func(q) == q:
                notes.append(f"[0x{tgt:x}] -> func 0x{q:x}")
            else:
                notes.append(f"0x{tgt:x}")
        elif op.type == X86_OP_IMM and insn.mnemonic.startswith(("call", "j")):
            f = containing_func(op.imm)
            if f == op.imm:
                notes.append(f"-> 0x{op.imm:x}"
                             + (f" [{end_of[f]-f}b]" if f in end_of else ""))
            elif f:
                notes.append(f"-> 0x{op.imm:x} (inside 0x{f:x})")
    return ("  ; " + ", ".join(notes)) if notes else ""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("va")
    ap.add_argument("-w", "--window", type=int, default=0,
                    help="instructions before/after the address")
    ap.add_argument("-n", "--count", type=int, default=0, help="max instructions")
    ap.add_argument("-s", "--skip", type=lambda x: int(x, 0), default=0,
                    help="byte offset into the function to start at")
    args = ap.parse_args()

    va = int(args.va, 0)
    f = containing_func(va)
    if f is None:
        print(f"0x{va:x}: not inside a .pdata function; disassembling raw")
        start, end = va, va + 0x200
    else:
        start, end = f, end_of[f]
        print(f"# func 0x{f:x} - 0x{end:x} ({end-f} bytes); query 0x{va:x}")

    blob = raw[va_to_off(start):va_to_off(end)]
    insns = list(md.disasm(blob, start))

    if args.window:
        idx = min(range(len(insns)), key=lambda i: abs(insns[i].address - va))
        lo, hi = max(0, idx - args.window), min(len(insns), idx + args.window + 1)
        insns = insns[lo:hi]
    elif args.skip:
        insns = [i for i in insns if i.address >= start + args.skip]
    if args.count:
        insns = insns[:args.count]

    for insn in insns:
        mark = ">>" if insn.address == va else "  "
        print(f"{mark} 0x{insn.address:x}  {insn.mnemonic:<9s}{insn.op_str}{annotate(insn)}")

main()
