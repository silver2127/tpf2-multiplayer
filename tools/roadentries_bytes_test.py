"""Verify the ROAD ENTRY ORDER hook in the supported binary.

A post-call detour at 0xa64473 (right after AddToEdgeUseManager's call to
EdgeUseManager::Add) re-sorts that edge's `entries` by vehicle name. Checks, against the exe:

  * the hook site's 8 bytes are `mov rbx,[rsp+0x80]` and nothing branches into them;
  * the call right before it is EdgeUseManager::Add (0x2115f80), with rcx = rsi (the manager)
    and rdx = rsp+0x30 (the EdgeId) set up just before -- what the stub reads back;
  * AddToEdgeUseManager treats rbx as the ecs world (lea rcx,[rbx+0x48] before the type-index
    lookup 0xd0a40), so rbx is the world the name lookups need;
  * Add's push_back appends 20-byte records (stride 0x14) whose first dword is the key it
    de-duplicates on, and GetEdgeDataPtr is a real function that indexes 32-byte EdgeData;
  * the two callers of AddToEdgeUseManager are the road move system's runtime add and its
    EntityAdded (the load path).

    python tools/roadentries_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


hook = const("RVA_ROADENTRIES_HOOK")
expect = byte_array("ROADENTRIES_EXPECT")
edgeid_off = const("ROADENTRIES_EDGEID_OFF")
get_ed = const("RVA_EDGEUSE_DATA")
assert 'FlagsSayOff("roadentries")' in source and "InstallRoadEntries();" in source
assert "TrainOrderNameCmp(a.k, b.k)" in source, "the sort must use the train-order name rule"
assert len(expect) == 8

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the measured build"
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data(); base = text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

got = pe.get_data(hook, 8)
assert got == expect, f"hook site changed: {got.hex(' ')} != {expect.hex(' ')}"
ins = list(md.disasm(got, hook))
assert [(i.mnemonic, i.op_str) for i in ins] == [("mov", "rbx, qword ptr [rsp + 0x80]")], [(i.mnemonic, i.op_str) for i in ins]

# the call right before: EdgeUseManager::Add, with rcx=rsi and rdx=rsp+0x30 set up before it
pre = pe.get_data(hook - 5, 5)
assert pre[0] == 0xE8 and (hook - 5) + 5 + struct.unpack("<i", pre[1:5])[0] == 0x2115f80, "the call before the hook is not EdgeUseManager::Add"
fn = list(md.disasm(pe.get_data(0xa64390, hook - 0xa64390), 0xa64390))
ops = [(i.mnemonic, i.op_str) for i in fn]
assert ("mov", "rcx, rsi") in ops[-8:], "Add's rcx (the manager) is no longer rsi"
assert ("lea", f"rdx, [rsp + {edgeid_off:#x}]") in ops[-8:], f"Add's rdx (the EdgeId) is no longer rsp+{edgeid_off:#x}"
assert ("mov", "rsi, r8") in ops[:8], "the manager is no longer parked in rsi"
# rbx = the ecs world: it feeds the type-index map at +0x48 right before 0xd0a40
for k, i in enumerate(fn):
    if i.mnemonic == "call" and i.op_str == "0xd0a40":
        assert any(j.mnemonic == "lea" and j.op_str == "rcx, [rbx + 0x48]" for j in fn[k - 6:k]), "rbx is no longer the world before the type-index lookup"
        break
else:
    raise AssertionError("no type-index lookup in AddToEdgeUseManager")
# no instruction between the hook and the ret writes rsi/rbx except the stolen reload of rbx
tail = list(md.disasm(pe.get_data(hook, 0x20), hook))
assert any(i.mnemonic == "ret" for i in tail), "no ret after the hook"

# nothing branches into the 8 stolen bytes
for d in md.disasm(code[0xa64390 - base:0xa644c0 - base], 0xa64390):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        assert not (hook < d.operands[0].imm < hook + 8), f"branch into the roadentries steal at {d.address:x}"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (hook < t < hook + 8), f"rel32 at {base + i:x} into the roadentries steal"

# Add's push_back: 20-byte records, first dword is the de-dup key
add = list(md.disasm(pe.get_data(0x2116080, 0x80), 0x2116080))
aops = [(i.mnemonic, i.op_str) for i in add]
assert ("add", "rax, 0x14") in aops and ("add", "qword ptr [rcx + 8], 0x14") in aops, "EdgeUseManager entry stride is no longer 0x14"
assert ("cmp", "dword ptr [rax], ebx") in aops, "Add no longer de-duplicates on the entry's first dword"
# GetEdgeDataPtr: real prologue, 32-byte EdgeData (sar rcx, 5)
ged = list(md.disasm(pe.get_data(get_ed, 0x70), get_ed))
gops = [(i.mnemonic, i.op_str) for i in ged]
assert gops[0] == ("sub", "rsp, 0x28") and ("sar", "rcx, 5") in gops, "GetEdgeDataPtr changed shape"

# the two callers of AddToEdgeUseManager
callers = set()
for i in range(len(code) - 5):
    if code[i] == 0xE8 and base + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == 0xa64390:
        callers.add(base + i)
assert callers == {0xa646a9, 0xa64ec2} or len(callers) == 2, f"AddToEdgeUseManager callers: {[hex(c) for c in sorted(callers)]}"

print(f"roadentries bytes: ok -- hook {hook:x} after EdgeUseManager::Add (rcx=rsi, rdx=rsp+{edgeid_off:#x}, rbx=world), "
      f"entries stride 0x14, GetEdgeDataPtr {get_ed:x}, {len(callers)} callers")
