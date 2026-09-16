"""Verify the ship and aircraft claim-order observer sites in the supported game
binary.

The offline half is tools/moveorder_test.cpp (the hashes). This half checks
what that cannot: that the five bytes slice_hook.cpp overwrites at the entry
of ecs::ShipMoveSystem::Update2 and ecs::AircraftMoveSystem::Update2 are still
`mov rax,rsp; push rbp; push rbx`, that the relay re-executes exactly those
three, that nothing branches into their middle, that Update2 is reached through
the vtable slot its Update fills n for by dividing the node vector's byte span
by 20, and that Update2 still walks that vector at [this+8] in 20-byte steps --
which is what the observer reads and the reason it does not reorder anything.
Constants are read out of the source.

    python tools/moveorder_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")
relay = (repo / "native/src/moveorderrelay_slice.asm").read_text(encoding="utf-8")
header = (repo / "native/src/moveorder.h").read_text(encoding="utf-8")


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


ship = const("RVA_SHIP_UPDATE2")
air = const("RVA_AIR_UPDATE2")
steal = int(re.search(r"MOVEORDER_STEAL\s*=\s*(\d+)", source)[1])
expect_ship = byte_array("MOVEORDER_EXPECT_SHIP")
expect_air = byte_array("MOVEORDER_EXPECT_AIR")
rec = int(re.search(r"MOVEORDER_REC\s*=\s*(\d+)", header)[1])
assert steal == 5 and rec == 20

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"
BASE = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

STOLEN = bytes.fromhex("488bc4 55 53".replace(" ", ""))
for name, rva, want in (("ship", ship, expect_ship), ("aircraft", air, expect_air)):
    got = pe.get_data(rva, len(want))
    assert got == want, f"{name} Update2 prologue changed: {got.hex(' ')} != {want.hex(' ')}"
    assert want[:steal] == STOLEN
    insns = list(md.disasm(want, BASE + rva))
    assert [(i.mnemonic, i.op_str) for i in insns[:3]] == [("mov", "rax, rsp"), ("push", "rbp"), ("push", "rbx")]
    assert sum(i.size for i in insns[:3]) == steal, "the steal does not end on an instruction boundary"
    assert insns[3].mnemonic == "push" and insns[3].op_str == "rsi", "site+5 is no longer `push rsi`"
    lea = next(i for i in insns if i.mnemonic == "lea")
    assert lea.op_str.startswith("rbp, [rax -"), "the frame is no longer anchored on rax"

# The relay re-executes exactly the stolen three, in order, after its pops and
# before the indirect jump, and touches nothing between the last pop and them.
body = re.search(r"MoveOrderBody MACRO.*?ENDM", relay, re.S)[0]
tail = body[body.rindex("pop  rax"):]
lines = [l.split(";")[0].strip() for l in tail.splitlines()]
lines = [re.sub(r"\s+", " ", l) for l in lines if l and l != "ENDM"]
assert lines == ["pop rax", "mov rax, rsp", "push rbp", "push rbx", "jmp qword ptr [resume]"], lines
assert "ShipOrderRelay PROC" in relay and "AirOrderRelay PROC" in relay
assert "MoveOrderBody 0, g_shipOrderResume" in relay and "MoveOrderBody 1, g_airOrderResume" in relay
saves = re.findall(r"movaps xmmword ptr \[rsp\+(\w+)\], (xmm\d)", body)
assert [r for _, r in saves] == [f"xmm{i}" for i in range(6)], "not all volatile xmm registers are saved"
assert re.search(r"mov\s+r9d, dword ptr \[rbx\+50h\]", body), "the engine's r8d (n) is not passed"

# Update2 is the last vtable slot of each system, and the slot before it is the
# Update that fills n: it downcasts to ecs::NodeList<4>, stores nodelist+8 (the
# vector) at this+8, divides the vector's byte span by 20 (imul by
# 0x6666666666666667, sar 3) and calls slot 0x58/8 = 11 through the vtable.
rdata = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".rdata")
rd = rdata.get_data()
for name, rva in (("ship", ship), ("aircraft", air)):
    at = rd.find(struct.pack("<Q", BASE + rva))
    assert at >= 0, f"{name} Update2 is in no vtable"
    assert rd.find(struct.pack("<Q", BASE + rva), at + 1) < 0, f"{name} Update2 is in two vtables"
    slot_rva = rdata.VirtualAddress + at
    update = struct.unpack_from("<Q", rd, at - 8)[0] - BASE
    code = list(md.disasm(pe.get_data(update, 0x1c0), BASE + update))
    ops = [(i.mnemonic, i.op_str) for i in code]
    assert ("movabs", "rax, 0x6666666666666667") in ops, f"{name} Update no longer divides the span by 20"
    assert ("mov", "qword ptr [rsi + 8], rdi") in ops and ("lea", "rdi, [rbx + 8]") in ops, \
        f"{name} Update no longer points this+8 at the node vector"
    assert ("call", "qword ptr [r9 + 0x58]") in ops, f"{name} Update no longer calls Update2 through slot 11"
    assert ("mov", "r8, rdx") in ops, f"{name} Update no longer passes n in r8"
    strings = [i for i in code if i.mnemonic == "lea" and "rip" in i.op_str]
    found = False
    for i in strings:
        tgt = i.address + i.size + i.operands[1].mem.disp - BASE
        try:
            s = pe.get_data(tgt, 64).split(b"\0")[0]
        except Exception:
            continue
        if b"NodeList<4>" in s:
            found = True
    assert found, f"{name} Update no longer downcasts to ecs::NodeList<4>"
    # ...and slot 11 of the vtable IS Update2: the vtable starts 11 slots
    # before, right after the RTTI locator pointer.
    assert (slot_rva - 11 * 8) >= rdata.VirtualAddress

# Update2 itself: n arrives in r8d and is what the loop bound holds; the loop
# reads [[this+8]] and steps 20 bytes.
ship_code = [(i.address - BASE, i.mnemonic, i.op_str) for i in md.disasm(pe.get_data(ship, 0x640), BASE + ship)]
assert (0xa6c267, "movsxd", "r12, r8d") in ship_code and (0xa6c357, "mov", "qword ptr [rbp + 0x20], r12") in ship_code
assert (0xa6c43a, "mov", "rax, qword ptr [r13 + 8]") in ship_code and (0xa6c43e, "mov", "rsi, qword ptr [rax]") in ship_code
assert (0xa6c441, "add", "rsi, r12") in ship_code and (0xa6c7e9, "add", "r12, 0x14") in ship_code
assert (0xa6c7f1, "cmp", "r15, qword ptr [rbp + 0x20]") in ship_code
assert (0xa6c26e, "mov", "r13, rcx") in ship_code and (0xa6c275, "mov", "qword ptr [rsp + 0x68], rdx") in ship_code
air_code = [(i.address - BASE, i.mnemonic, i.op_str) for i in md.disasm(pe.get_data(air, 0x800), BASE + air)]
assert (0xa2bce9, "movsxd", "rsi, r8d") in air_code and (0xa2bdde, "mov", "qword ptr [rbp - 0x28], rsi") in air_code
assert (0xa2beb3, "mov", "rax, qword ptr [r13 + 8]") in air_code and (0xa2beb7, "mov", "rsi, qword ptr [rax]") in air_code
assert (0xa2c434, "add", "rdx, 0x14") in air_code and (0xa2c43c, "cmp", "r14, qword ptr [rbp - 0x28]") in air_code
assert (0xa2bcf2, "mov", "r13, rcx") in air_code and (0xa2bcef, "mov", "r15, rdx") in air_code
# ...and the arbitration the finding is about: IsReserved then Reserve, no shuffle.
RESERVE, IS_RESERVED = 0x21150b0, 0x2114e70
for name, code_ in (("ship", ship_code), ("aircraft", air_code)):
    calls = [o for a, m, o in code_ if m == "call"]
    assert f"{BASE + IS_RESERVED:#x}" in calls and f"{BASE + RESERVE:#x}" in calls, f"{name}: reservation calls moved"
    assert not any(m == "imul" and "0xbc8f" in o for a, m, o in code_), f"{name}: a minstd_rand appeared -- a shuffle?"

# Nothing may branch into the middle of a stolen instruction.
text_sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text_sec.get_data()
base = text_sec.VirtualAddress
inside = []
for i in range(len(code) - 5):
    if code[i] not in (0xE8, 0xE9):
        continue
    tgt = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
    for s in (ship, air):
        if s < tgt < s + steal:
            inside.append((base + i, tgt))
for s in (ship, air):
    lo, hi = s - 0x200, s + 0x800
    for ins in md.disasm(code[lo - base:hi - base], BASE + lo):
        if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == X86_OP_IMM:
            t = ins.operands[0].imm - BASE
            if s < t < s + steal:
                inside.append((ins.address - BASE, t))
assert not inside, f"something branches into a stolen instruction: {[(hex(a), hex(b)) for a, b in inside]}"

print(f"PASS: {steal} bytes at {ship:x} and {air:x} are `mov rax,rsp; push rbp; push rbx`, the relay "
      f"re-executes exactly those after its pops, both Update2s are vtable slot 11 called by an Update "
      f"that stores the NodeList<4> vector at this+8 and passes its span/20 in r8d, both loops step "
      f"that vector by {rec} bytes and arbitrate with IsReserved/Reserve and no shuffle, and nothing "
      f"branches into the stolen bytes")
