"""Verify the road free-space hook sites in the supported game binary.

The offline half of this patch is tools/roadspace_test.cpp (the arithmetic).
This half checks what that test cannot: that the bytes slice_hook.cpp is about
to overwrite in EdgeUseManager::GetUsedSpace and its filtered sibling are still
the ones they were measured on, that each five-byte steal is exactly one whole
instruction with nothing branching into its middle, that the two engine helpers
the replacement calls are the ones the comments describe, and that the loop
being replaced really is the single-precision, vector-order sum the finding
names. Constants are read out of the source, so a drifting RVA fails here
instead of in a running game.

    python tools/roadspace_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP, X86_REG_RSP

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


site_a = const("RVA_ROADSPACE_A")
site_b = const("RVA_ROADSPACE_B")
get_data = const("RVA_EDGEUSE_DATA")
get_pos = const("RVA_EDGEUSE_POS")
steal = int(re.search(r"ROADSPACE_STEAL\s*=\s*(\d+)", source)[1])
expect_a = byte_array("ROADSPACE_EXPECT_A")
expect_b = byte_array("ROADSPACE_EXPECT_B")
expect_data = byte_array("EDGEUSE_DATA_EXPECT")
expect_pos = byte_array("EDGEUSE_POS_EXPECT")
cap = int(re.search(r"ROADSPACE_MAX_TERMS\s*=\s*(\d+)",
                    (repo / "native/src/roadspace.h").read_text(encoding="utf-8"))[1])

assert steal == 5, "the near detour is a 5-byte jmp rel32"
assert len(expect_a) >= steal and len(expect_b) >= steal

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"
BASE = pe.OPTIONAL_HEADER.ImageBase

for name, rva, want in (("GetUsedSpace", site_a, expect_a), ("GetUsedSpace(skip)", site_b, expect_b),
                        ("GetEdgeDataPtr", get_data, expect_data), ("GetPos", get_pos, expect_pos)):
    got = pe.get_data(rva, len(want))
    assert got == want, f"{name} at {rva:x} changed: {got.hex(' ')} != {want.hex(' ')}"

# The call inside the first prologue resolves to GetEdgeDataPtr -- the same
# check InstallRoadSpace makes at run time.
assert expect_a[13] == 0xE8
rel = struct.unpack("<i", expect_a[14:18])[0]
assert site_a + 18 + rel == get_data, f"call resolves to {site_a + 18 + rel:x}, not {get_data:x}"

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def first_insn(rva):
    return next(md.disasm(pe.get_data(rva, 16), BASE + rva))


# Each steal is exactly one instruction: a `mov [rsp+disp8], reg` -- rsp-relative,
# so it runs unchanged from the trampoline (which is entered by a jmp, with the
# same rsp), and not rip-relative.
for name, rva in (("GetUsedSpace", site_a), ("GetUsedSpace(skip)", site_b)):
    ins = first_insn(rva)
    assert ins.size == steal, f"{name}: first instruction is {ins.size} bytes, steal is {steal}"
    assert ins.mnemonic == "mov" and ins.operands[0].type == X86_OP_MEM
    assert ins.operands[0].mem.base == X86_REG_RSP, f"{name}: stolen instruction is not rsp-relative"
    for op in ins.operands:
        assert not (op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP)

# The helpers' ABIs, as far as bytes can say: GetEdgeDataPtr indexes with the
# int at [rdx] (the EdgeId) and returns null through `xor eax,eax`; GetPos
# tests edx (the component index) and, on the forward path, subtracts the
# position from xmm2 (the edge length) -- which is what makes xmm2 the third
# argument.
data_code = list(md.disasm(pe.get_data(get_data, 0x8f), BASE + get_data))
assert any(i.mnemonic == "xor" and i.op_str == "eax, eax" for i in data_code), "GetEdgeDataPtr null path moved"
assert any(i.mnemonic == "cmp" and "[rcx + r8*4]" in i.op_str for i in data_code), "GetEdgeDataPtr index check moved"
pos_code = list(md.disasm(pe.get_data(get_pos, 0x102), BASE + get_pos))
assert any(i.mnemonic == "subss" and i.op_str == "xmm2, xmm0" for i in pos_code), "GetPos no longer takes the length in xmm2"

# The loop the plain overload is replacing: the per-term `subss xmm1,xmm0`
# (hi - lo), the single-precision accumulate `addss xmm1,xmm7`, the 20-byte
# stride, and the two GetPos calls per surviving entry with xmm2 = [rsi] = the
# edge length reloaded each time.
loop = list(md.disasm(pe.get_data(site_a, 0x137), BASE + site_a))
text = [(i.address - BASE, i.mnemonic, i.op_str) for i in loop]
assert (0x2117448, "subss", "xmm1, xmm0") in text, "the hi - lo subtraction moved"
assert (0x2117450, "addss", "xmm1, xmm7") in text, "the single-precision accumulate moved"
assert (0x211744c, "add", "rdi, 0x14") in text, "the entry stride is no longer 20"
assert (0x2117457, "cmp", "rdi, r14") in text and (0x211745a, "jne", f"{BASE + 0x21173b0:#x}") in text
pos_calls = [a for a, m, o in text if m == "call" and o == f"{BASE + get_pos:#x}"]
assert len(pos_calls) == 3, f"expected 3 GetPos calls in the plain loop, found {len(pos_calls)}"
length_loads = [a for a, m, o in text if m == "movss" and o == "xmm2, dword ptr [rsi]"]
assert len(length_loads) == 3, "the length is no longer reloaded into xmm2 before each GetPos"
assert (0x21173ba, "movzx", "ebx, byte ptr [rdi + 0x10]") in text, "the entry's forward byte moved"
assert (0x2117403, "comiss", "xmm6, xmm1") in text and (0x2117444, "maxss", "xmm0, xmm6") in text, "the clipping moved"

# The filtered overload: the direction byte read out of the EdgeId, the
# std::function target at +0x38, its _Do_call at vtable+0x10, and the throw on
# a null target, which the detour hands back to the engine rather than skipping.
loopb = [(i.address - BASE, i.mnemonic, i.op_str)
         for i in md.disasm(pe.get_data(site_b, 0x210), BASE + site_b)]
assert (0x2117174, "cmp", "byte ptr [rbx + 8], 0") in loopb, "the EdgeId direction read moved"
assert (0x21171cc, "mov", "rcx, qword ptr [r14 + 0x38]") in loopb, "std::function _Getimpl is no longer +0x38"
assert (0x21171ea, "call", "qword ptr [rax + 0x10]") in loopb, "_Do_call is no longer vtable+0x10"
assert (0x21171e2, "lea", "r8, [rsp + 0x68]") in loopb and (0x21171e7, "mov", "rdx, rbx") in loopb
assert (0x2117246, "addss", "xmm7, xmm1") in loopb and (0x211732e, "addss", "xmm7, xmm1") in loopb
assert sum(1 for a, m, o in loopb if m == "call" and o == f"{BASE + get_pos:#x}") == 6

# The consumer that turns one ULP into a junction decision.
consumer = 0x2214bcd
ins = first_insn(consumer)
assert ins.mnemonic == "call" and ins.operands[0].imm == BASE + site_a, "the MotionCalculator call site moved"
after = [(i.address - BASE, i.mnemonic, i.op_str)
         for i in md.disasm(pe.get_data(0x2214e76, 8), BASE + 0x2214e76)]
assert after[0][1:] == ("comiss", "xmm12, xmm6") and after[1][1] == "jbe", "the compare on the space left moved"

# Nothing may branch into the middle of a stolen instruction. rel32 targets
# are found by a byte scan of .text; short jumps by decoding around each site.
text_sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text_sec.get_data()
base = text_sec.VirtualAddress
inside = []
for i in range(len(code) - 5):
    if code[i] not in (0xE8, 0xE9):
        continue
    tgt = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
    for s in (site_a, site_b):
        if s < tgt < s + steal:
            inside.append((base + i, tgt))
for s in (site_a, site_b):
    lo, hi = s - 0x200, s + 0x400
    for ins in md.disasm(code[lo - base:hi - base], BASE + lo):
        if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm - BASE
            if s < t < s + steal:
                inside.append((ins.address - BASE, t))
assert not inside, f"something branches into a stolen instruction: {[(hex(a), hex(b)) for a, b in inside]}"

print(f"PASS: {steal}-byte steals at {site_a:x} and {site_b:x} are the measured whole instructions, "
      f"the prologue call resolves to GetEdgeDataPtr at {get_data:x}, GetPos at {get_pos:x} takes the "
      f"length in xmm2, both loops still accumulate in single precision in vector order over 20-byte "
      f"entries, the consumer at {consumer:x} still compares the result bare, nothing branches into "
      f"the stolen bytes, and the sort holds {cap} terms")
