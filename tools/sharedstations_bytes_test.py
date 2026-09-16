"""Verify the line editor's owner gate in the supported game binary.

The shared-stations patch rests on one claim: the ONLY thing stopping company B
from putting company A's station on B's line is the comparison at 0x609631
inside UI::_anon_46B7A670::StationFilter::IsValid. This checks what a running
game cannot be asked right now:

  * the 46 bytes slice_hook.cpp guards are still the owner test it describes,
    and the two rel32 calls inside them still resolve to the engine accessor
    and to GetComponentPtr<PlayerOwned>;
  * the five stolen bytes are exactly the `cmp` and the `je`, two whole
    instructions, with nothing branching into their middle and no rip-relative
    or rel32 operand (so a jmp may replace them);
  * both labels the stub jumps to are the function's own accept and reject
    tails, and neither reads any register the stub clobbers;
  * StationFilter::IsValid really is vftable slot 1 of the filter the line
    editor builds, and really does call IsValidInput -- i.e. the patched
    comparison is on the "add station" click path and nowhere else;
  * make_cmd::UpdateLine and its sim-thread handler, which our replication
    replays with a foreign station id, contain no PlayerOwned read at all, so
    nothing downstream strips the stop again.

Constants are read out of the source, so a drifting RVA fails here instead of
in a running game.

    python tools/sharedstations_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


guard = const("RVA_SHAREDSTATIONS_GUARD")
site = const("RVA_SHAREDSTATIONS_SITE")
accept = const("RVA_SHAREDSTATIONS_ACCEPT")
reject = const("RVA_SHAREDSTATIONS_REJECT")
get_engine = const("RVA_SS_GETENGINE")
get_owned = const("RVA_SS_GETPLAYEROWNED")
steal = int(re.search(r"SHAREDSTATIONS_STEAL\s*=\s*(\d+)", source)[1])
expect = byte_array("SHAREDSTATIONS_EXPECT")

assert steal == 5, "the near detour is a 5-byte jmp rel32"
assert "sharedstations=0" in source, "the kill switch is gone from the source"
assert 'FlagsSayOff("sharedstations")' in source, "the install no longer consults the kill switch"
assert accept < guard < site < reject, "the labels are no longer laid out as the finding says"

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"
BASE = pe.OPTIONAL_HEADER.ImageBase

got = pe.get_data(guard, len(expect))
assert got == expect, f"the owner test at {guard:x} changed: {got.hex(' ')} != {expect.hex(' ')}"
assert site == guard + 0x21, "the comparison moved inside the guarded window"

# The two calls inside the guarded window -- the same checks InstallSharedStations
# makes at run time, so a byte match that landed on another function cannot pass.
assert expect[4] == 0xE8
rel = struct.unpack("<i", expect[5:9])[0]
assert guard + 9 + rel == get_engine, f"first call resolves to {guard + 9 + rel:x}, not {get_engine:x}"
assert expect[17] == 0xE8
rel = struct.unpack("<i", expect[18:22])[0]
assert guard + 22 + rel == get_owned, f"second call resolves to {guard + 22 + rel:x}, not {get_owned:x}"

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def insns(rva, n):
    return list(md.disasm(pe.get_data(rva, n), BASE + rva))


# The five stolen bytes are two whole instructions: `cmp eax,[rbx+0x28]` and a
# short `je` to the accept label. Neither is rip-relative and neither is a
# rel32 call, so a jmp rel32 may replace both.
stolen = insns(site, steal)
assert len(stolen) == 2, f"the steal covers {len(stolen)} instructions, not 2"
assert sum(i.size for i in stolen) == steal, "the steal does not end on an instruction boundary"
cmp_ins, je_ins = stolen
assert cmp_ins.mnemonic == "cmp" and cmp_ins.op_str == "eax, dword ptr [rbx + 0x28]", cmp_ins.op_str
assert je_ins.mnemonic == "je" and je_ins.operands[0].imm - BASE == accept, "the je no longer targets accept"
for ins in stolen:
    for op in ins.operands:
        assert not (op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP), "stolen byte is rip-relative"

# GetComponentPtr<PlayerOwned> really is what the site compares: the instruction
# before the steal loads eax from the pointer it returned, and the one before
# that is the null/negative accept pair.
head = insns(guard + 0x16, 0x21 - 0x16)
text = [(i.address - BASE, i.mnemonic, i.op_str) for i in head]
assert (guard + 0x16, "test", "rax, rax") in text, "the null-owner check moved"
assert (guard + 0x1b, "mov", "eax, dword ptr [rax]") in text, "PlayerOwned.player is no longer at +0"
assert (guard + 0x1f, "js", f"{BASE + accept:#x}") in text, "the owner < 0 accept moved"

# The two tails the stub jumps to: both set eax themselves and then unwind the
# same frame, so the stub may clobber rax/rcx/rdx and must preserve rbx and rsp.
acc = [(i.mnemonic, i.op_str) for i in insns(accept, 11)]
assert acc[0] == ("mov", "eax, 1"), acc[0]
assert acc[1] == ("add", "rsp, 0x20") and acc[2] == ("pop", "rbx") and acc[3] == ("ret", ""), acc
rej = [(i.mnemonic, i.op_str) for i in insns(reject, 11)]
assert rej[0] == ("xor", "eax, eax"), rej[0]
assert rej[1] == ("add", "rsp, 0x20") and rej[2] == ("pop", "rbx") and rej[3] == ("ret", ""), rej

# ...and the frame the stub inherits: `push rbx; sub rsp,0x20` leaves rsp
# 16-aligned at the site, which is what makes the stub's call ABI-correct.
filt = const("RVA_SHAREDSTATIONS_GUARD") - 0x40  # 0x6095d0, the function entry
entry = [(i.mnemonic, i.op_str) for i in insns(filt, 8)]
assert entry[0] == ("push", "rbx") and entry[1] == ("sub", "rsp, 0x20"), entry
assert filt == 0x6095d0

# This function is the line editor's station filter: vftable slot 1 of
# UI::_anon_46B7A670::StationFilter, and it calls IsValidInput first.
body = [(i.mnemonic, i.op_str) for i in insns(filt, site - filt)]
calls = [o for m, o in body if m == "call"]
assert f"{BASE + 0x609b00:#x}" in calls, "IsValid no longer calls AddStationInputComponentChecker::IsValidInput"
vt = pe.get_data(0x3010848, 16)
slot0, slot1 = struct.unpack("<QQ", vt)
assert slot1 - BASE == filt, f"StationFilter vftable slot 1 is {slot1 - BASE:x}, not {filt:x}"

# Only classes 0 and 1 reach the owner test; 2 and 3 accept outright and
# anything else rejects. That is what bounds the patch to station/stop/edge
# selections.
cls = [(i.address - BASE, i.mnemonic, i.op_str) for i in insns(filt + 0x27, 0x19)]
assert (0x6095f7, "test", "edx, edx") in cls and (0x6095f9, "js", f"{BASE + reject:#x}") in cls
assert (0x6095fb, "cmp", "edx, 1") in cls and (0x6095fe, "jle", f"{BASE + guard:#x}") in cls
assert (0x609600, "cmp", "edx, 3") in cls and (0x609603, "jg", f"{BASE + reject:#x}") in cls

# The stub slice_hook.cpp hand-assembles, rebuilt from the source's own opcode
# list and disassembled: it must be exactly the sequence the comment claims,
# jump over the reject arm by the right displacement, and touch no register the
# tails read. A typo'd opcode here is a jump into the middle of an instruction
# in a running game, which no other test would catch.
emit = re.search(r"size_t n = 0;\s*(.*?)FlushInstructionCache", source, re.S)[1]
stub = bytearray()
for line in emit.splitlines():
    line = re.sub(r"//.*", "", line)
    for b in re.findall(r"stub\[n\+\+\]\s*=\s*(0x[0-9A-Fa-f]{2})", line):
        stub += bytes([int(b, 16)])
    m = re.search(r"memcpy\(stub \+ n, &(\w+), 8\)", line)
    if m:
        stub += struct.pack("<Q", {"helper": 0x7FF700001234,
                                   "accept": BASE + accept,
                                   "reject": BASE + reject}[m[1]])
want = [("mov", "ecx, eax"), ("mov", "edx, dword ptr [rbx + 0x28]"), ("sub", "rsp, 0x20"),
        ("movabs", "rax, 0x7ff700001234"), ("call", "rax"), ("add", "rsp, 0x20"),
        ("test", "al, al"), ("jne", None),
        ("movabs", f"rax, {BASE + reject:#x}"), ("jmp", "rax"),
        ("movabs", f"rax, {BASE + accept:#x}"), ("jmp", "rax")]
decoded = list(md.disasm(bytes(stub), 0x10000))
assert sum(i.size for i in decoded) == len(stub), "the stub does not decode cleanly end to end"
assert len(decoded) == len(want), f"the stub is {len(decoded)} instructions, not {len(want)}"
for ins, (mn, ops) in zip(decoded, want):
    assert ins.mnemonic == mn, f"stub: {ins.mnemonic} {ins.op_str} != {mn} {ops}"
    if ops is not None:
        assert ins.op_str == ops, f"stub: {ins.mnemonic} {ins.op_str} != {mn} {ops}"
assert decoded[7].operands[0].imm == decoded[10].address, "the jne does not skip exactly the reject arm"
# rsp is 16-aligned at the site (push rbx + sub rsp,0x20 above), and the stub's
# own sub keeps it so: the call sees the alignment the ABI requires.
assert decoded[2].operands[1].imm % 16 == 0, "the stub's shadow space breaks the 16-byte alignment"

# Nothing may branch into the middle of the stolen instructions: rel32 targets
# by a byte scan of .text, short jumps by decoding around the site.
text_sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text_sec.get_data()
tbase = text_sec.VirtualAddress
inside = []
for i in range(len(code) - 5):
    if code[i] not in (0xE8, 0xE9):
        continue
    tgt = tbase + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
    if site < tgt < site + steal:
        inside.append((tbase + i, tgt))
lo, hi = site - 0x400, site + 0x400
for ins in md.disasm(code[lo - tbase:hi - tbase], BASE + lo):
    if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == X86_OP_IMM:
        t = ins.operands[0].imm - BASE
        if site < t < site + steal:
            inside.append((ins.address - BASE, t))
assert not inside, f"something branches into a stolen instruction: {[(hex(a), hex(b)) for a, b in inside]}"

# The command layer does not repeat the gate: neither make_cmd::UpdateLine nor
# the sim-thread handler the dispatch table reaches for variant tag 5 touches
# the PlayerOwned type descriptor or either accessor, so a replayed updateLine
# carrying a foreign station id keeps it. (0x41d1628 is the descriptor, +8 the
# name pointer the type_info hash is taken from.)
PO_DESC = 0x41D1628


def owner_reads(rva, size):
    """(descriptor leas, accessor calls) inside a byte range."""
    leas, calls = [], []
    for ins in insns(rva, size):
        if ins.mnemonic == "lea" and ins.operands[1].type == X86_OP_MEM \
                and ins.operands[1].mem.base == X86_REG_RIP:
            tgt = ins.address + ins.size + ins.operands[1].mem.disp - BASE
            if tgt in (PO_DESC, PO_DESC + 8):
                leas.append(ins.address - BASE)
        if ins.mnemonic == "call" and ins.operands[0].type == X86_OP_IMM:
            if ins.operands[0].imm - BASE in (get_owned, 0xC5E20):
                calls.append(ins.address - BASE)
    return leas, calls


for name, rva, size in (("make_cmd::UpdateLine", 0x9DF4E0, 0x230),
                        ("UpdateLine handler", 0x9D9FD0, 0x340)):
    leas, calls = owner_reads(rva, size)
    assert not leas and not calls, f"{name} now reads an owner at {[hex(a) for a in leas + calls]}"

# LineSystem::EntityAdded does read one owner, and it must stay the line's own:
# a single GetComponent<PlayerOwned>(engine, lineEntity) whose arguments are the
# function's own (r13, r14) -- it feeds the per-player line index (the same one
# ComponentAddedToEntity maintains at 0xa415b0), never a stop's station group.
leas, calls = owner_reads(0xA43400, 0x200)
assert not leas and calls == [0xA43457], f"LineSystem::EntityAdded owner reads changed: {calls}"
setup = [(i.mnemonic, i.op_str) for i in insns(0xA43451, 6)]
assert setup[0] == ("mov", "rdx, r14") and setup[1] == ("mov", "rcx, r13"), setup

# The dispatch table entry the replayed updateLine actually lands on (variant
# tag 5 -- CmdData order: SetGameSpeed, SetCalendarSpeed, UpdateLogo, CreateLine,
# DeleteLine, UpdateLine, ...) still reaches that handler.
thunk = struct.unpack("<Q", pe.get_data(0x30B10C0 + 8 * 5, 8))[0] - BASE
first = next(md.disasm(pe.get_data(thunk, 16), BASE + thunk))
assert first.mnemonic == "jmp" and first.operands[0].imm - BASE == 0x9D9FD0, \
    f"dispatch tag 5 now reaches {first.op_str}, not the UpdateLine handler"

print(f"PASS: the line editor's owner gate is the {steal}-byte `cmp eax,[rbx+0x28]` / `je` at "
      f"{site:x} in StationFilter::IsValid ({filt:x}, vftable slot 1), reached only for "
      f"ValidAddStationSelection 0 and 1; its calls still resolve to {get_engine:x} and "
      f"{get_owned:x}; accept {accept:x} and reject {reject:x} are the function's own tails; "
      f"nothing branches into the stolen bytes; and neither make_cmd::UpdateLine nor the "
      f"handler at 9d9fd0 (dispatch tag 5) reads an owner")
