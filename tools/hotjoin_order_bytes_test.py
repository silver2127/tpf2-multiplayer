"""Verify the HOT-JOIN ORDER sites (native/src/slice/hotjoin_order.inl) in the
supported game binary.

For each site: the bytes the .inl expects are the exe's; the
stolen instruction(s) end on an instruction boundary, contain no branch or
rip-relative operand, and are the instruction the relay re-executes; nothing in
the function branches into the middle of the steal; and the vector the relay
hands HotJoinSort is the one the engine reads next (candidates: the size is
taken from [rbp-0x69]-[rbp-0x71] right after; departures/arrivals: `lea rdx`
of the same slot is the signal's payload).

    python tools/hotjoin_order_bytes_test.py
"""
from pathlib import Path
import re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM
from slice_source import slice_source

repo = Path(__file__).resolve().parents[1]
source = slice_source(repo)
relay = (repo / "native/src/hotjoinrelay_slice.asm").read_text(encoding="utf-8")

sites = re.findall(r'\{ "(\w+)",\s*(0x[0-9a-f]+), (\d+), \{ ([^}]*) \}, (\d+), (\w+), &(\w+) \}', source)
assert [s[0] for s in sites] == ["candidates", "departures", "arrivals", "idle", "capacity"], sites

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"
BASE = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# function ranges (the three functions the sites live in) for the branch-into check
FUNCS = {"candidates": (0x9279f0, 0x928000), "departures": (0xa7c920, 0xa7cb00), "arrivals": (0xa59450, 0xa59b00), "idle": (0xa86760, 0xa86c80),
         "capacity": (0x2122fd0, 0x2123800)}
VECTOR = {"candidates": "[rbp-71h]", "departures": "[rbx+78h+28h]", "arrivals": "[rbx+78h+68h]", "idle": "[r13+18h]", "capacity": "[r13+120h]"}
STOLEN_ASM = {"candidates": ["mov dword ptr [rbp-79h], 1"], "departures": ["lea rdx, [rsp+28h]"], "arrivals": ["lea rdx, [rsp+68h]"],
              "idle": ["mov rdx, qword ptr [r13+20h]", "sub rdx, qword ptr [r13+18h]"],
              "capacity": ["mov rdi, qword ptr [r13+120h]"]}

for idx, (name, rva_s, steal_s, expect_s, elen_s, relay_name, resume) in enumerate(sites):
    rva, steal, elen = int(rva_s, 16), int(steal_s), int(elen_s)
    expect = bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", expect_s))
    assert len(expect) == elen, name
    got = pe.get_data(rva, elen)
    assert got == expect, f"{name}: exe bytes {got.hex(' ')} != expected {expect.hex(' ')}"
    insns = list(md.disasm(pe.get_data(rva, 32), BASE + rva))
    covered, n = 0, 0
    while covered < steal:
        i = insns[n]
        assert i.mnemonic not in ("call", "jmp") and not i.mnemonic.startswith("j"), f"{name}: branch in the steal"
        for op in i.operands:
            assert not (op.type == X86_OP_MEM and op.mem.base == 41), f"{name}: rip-relative operand in the steal"  # 41 = RIP
        covered += i.size; n += 1
    assert covered == steal, f"{name}: the steal does not end on an instruction boundary"
    # the relay replays exactly the stolen instruction, then jumps to its resume slot
    body = re.search(rf"{relay_name} PROC(.*?){relay_name} ENDP", relay, re.S)[1]
    lines = [re.sub(r"\s+", " ", l.split(";")[0].strip()) for l in body.splitlines()]
    lines = [l for l in lines if l]
    assert lines == [f"HotJoinBody {idx}, {VECTOR[name]}", *STOLEN_ASM[name], f"jmp qword ptr [{resume}]"], (name, lines)
    stolen = [f"{i.mnemonic} {i.op_str}" for i in insns[:n]]
    want = {"candidates": ["mov dword ptr [rbp - 0x79], 1"], "departures": ["lea rdx, [rsp + 0x28]"],
            "arrivals": ["lea rdx, [rsp + 0x68]"],
            "idle": ["mov rdx, qword ptr [r13 + 0x20]", "sub rdx, qword ptr [r13 + 0x18]"],
            "capacity": ["mov rdi, qword ptr [r13 + 0x120]"]}[name]
    assert stolen == want, (name, stolen)
    # nothing in the function jumps into the middle of the steal
    lo, hi = FUNCS[name]
    code = pe.get_data(lo, hi - lo)
    for i in md.disasm(code, BASE + lo):
        if i.mnemonic.startswith("j") or i.mnemonic == "call":
            op = i.operands[0] if i.operands else None
            if op is not None and op.type == 2:  # immediate target
                t = op.imm - BASE
                assert not (rva < t < rva + steal), f"{name}: {hex(i.address)} branches into the steal"
    # the vector the relay passes is the one read next
    after = list(md.disasm(pe.get_data(rva + steal, 24), BASE + rva + steal))
    text = " ; ".join(f"{i.mnemonic} {i.op_str}" for i in after[:3])
    if name == "capacity":
        # lea rbx,[rdi+0x240] -- the SimEntityData right after the nine maps (D+0x240)
        assert after[0].mnemonic == "lea" and after[0].op_str == "rbx, [rdi + 0x240]", text
    elif name == "idle":
        # sar rdx,2 ; lea r8,[rbp-0x69] ; lea rcx,[rbp-9] ; call 0xa85fe0 -- the list's count feeds the batch
        assert after[0].mnemonic == "sar" and after[0].op_str == "rdx, 2", text
    elif name == "candidates":
        # call 0x23827c0 ; mov rcx,[rbp-0x69] ; mov rdx,[rbp-0x71]  (end, begin)
        assert "qword ptr [rbp - 0x69]" in text and "qword ptr [rbp - 0x71]" in text, text
    else:
        assert after[0].mnemonic == "mov" and after[0].op_str == "rcx, qword ptr [rcx + 0x10]", text
        assert after[1].mnemonic == "call", text

# the relay saves the flags, the volatile xmm registers and hands (site, &vector) over
macro = re.search(r"HotJoinBody MACRO.*?ENDM", relay, re.S)[0]
assert "pushfq" in macro and "popfq" in macro
saves = re.findall(r"movaps xmmword ptr \[rsp\+(\w+)\], (xmm\d)", macro)
assert [r for _, r in saves] == [f"xmm{i}" for i in range(6)]
pushes = re.findall(r"^\s*push\s+(\w+)", macro, re.M)
pops = re.findall(r"^\s*pop\s+(\w+)", macro, re.M)
assert pushes == list(reversed(pops)) and len(pushes) == 14, (pushes, pops)
assert "mov  ecx, site" in macro and "lea  rdx, vecExpr" in macro
print("hotjoin order sites OK:", ", ".join(f"{s[0]} {s[1]}" for s in sites))
