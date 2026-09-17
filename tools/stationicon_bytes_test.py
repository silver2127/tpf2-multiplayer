"""Verify the STATION ICON COLOUR hook in the supported binary.

A post-call detour at 0x5e38e1 (right after DoStep wraps the item content into its
ItemButton, call 0x2251620) tags the button root with a company style class. Checks, against the exe:

  * the hook site's 6 bytes are `mov rdi,rax ; xor r12d,r12d` as the source records, so a
    5-byte jmp + 1 NOP replaces them and the two instructions are re-run in the stub;
  * the immediately preceding call is the ItemButton wrap 0x2251620 (so rax at the hook
    is the button root), and the call before that (0x5e38cb) is FUN_5e45d0;
  * nothing branches into the 6 stolen bytes;
  * the StationGroup type_info string is at RVA_TI_STATIONGROUP + 0x10;
  * the accessors (type index, GetComponentPtr<PlayerOwned>) are real function starts, and
    the asserting GetComponentPtr<StationGroup> is NOT used (it crash-dumped on towns).

    python tools/stationicon_bytes_test.py
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


hook = const("RVA_ICON_STN_HOOK")
expect = byte_array("ICON_STN_EXPECT")
ti_sg = const("RVA_TI_STATIONGROUP")
get_ti = const("RVA_GET_TYPEINDEX")
get_po = const("RVA_GET_PLAYEROWNED")

assert 'FlagsSayOff("stationicon")' in source and "InstallStationIconColor();" in source
assert "RVA_GET_COMPONENT_SG" not in source and "(GetComp)" not in source, "the asserting GetComponentPtr<StationGroup> (0x149290 -> 0xd0920) is back: it crash-dumps on towns/industries"
assert "TrainOrderSlot(engine, entity, ti)" in source, "the station-group walk must use the non-asserting slot scan"
assert len(expect) == 6

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the measured build"
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data(); base = text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

got = pe.get_data(hook, 6)
assert got == expect, f"hook site changed: {got.hex(' ')} != {expect.hex(' ')}"
ins = list(md.disasm(got, hook))
assert [(i.mnemonic, i.op_str) for i in ins] == [("mov", "rdi, rax"), ("xor", "r12d, r12d")], [(i.mnemonic, i.op_str) for i in ins]

# the call right before the hook is the ItemButton wrap (rax = the button root), and the
# item-content build FUN_5e45d0 is the call at 0x5e38cb just above it
pre = pe.get_data(hook - 5, 5)
assert pre[0] == 0xE8, f"no call right before the hook: {pre.hex(' ')}"
callee = (hook - 5) + 5 + struct.unpack("<i", pre[1:5])[0]
assert callee == 0x2251620, f"pre-call resolves to {callee:x}, not the ItemButton wrap 0x2251620"
c2 = pe.get_data(0x5e38cb, 5)
assert c2[0] == 0xE8 and 0x5e38cb + 5 + struct.unpack("<i", c2[1:5])[0] == 0x5e45d0, "FUN_5e45d0 call moved"
# the je at 0x5e38c0 lands at 0x5e38e9, past the stolen bytes (checked by the scan below)

# nothing branches into the 6 stolen bytes (whole DoStep + a wide rel32 scan)
for d in md.disasm(code[0x5e2dc0 - base:0x5e4000 - base], 0x5e2dc0):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        assert not (hook < d.operands[0].imm < hook + 6), f"branch into station-icon steal at {d.address:x}"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (hook < t < hook + 6), f"rel32 at {base + i:x} into the station-icon steal"

# StationGroup type_info string
s = pe.get_data(ti_sg + 0x10, 40).split(b"\0")[0]
assert s == b".?AUStationGroup@component@ecs@@", f"RVA_TI_STATIONGROUP is {s!r}"

# accessors are real function prologues
for rva, name in ((get_ti, "GetTypeIndex 0xd0a40"), (get_po, "GetComponentPtr<PlayerOwned> 0x472900")):
    p = pe.get_data(rva, 4)
    assert p[0] in (0x48, 0x4c, 0x40, 0x53, 0x55, 0x56, 0x57), f"{name} prologue {p.hex(' ')}"

# ---- the class on the icon element: the content-builder prologue and the two addStyleClass calls ----
content = const("RVA_ICON_CONTENT_FN")
pro = byte_array("ICON_CONTENT_PROLOGUE")
assert len(pro) == 15 and pe.get_data(content, 15) == pro, f"content-builder prologue changed: {pe.get_data(content, 15).hex(' ')}"
pins = list(md.disasm(pro, content))
assert sum(i.size for i in pins) == 15 and pins[0].mnemonic == "mov" and pins[-1].mnemonic == "sub", [(i.mnemonic, i.op_str) for i in pins]
assert not any("rip" in i.op_str for i in pins), "a rip-relative operand in the stolen prologue"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (content < t < content + 15), f"rel32 at {base + i:x} into the content-builder steal"
addclass = const("RVA_ADD_STYLE_CLASS")
for cname, ename, fn_lo, fn_hi in (("RVA_STNICON_CLASS_CALL", "STNICON_CLASS_EXPECT", 0x5e0070, 0x5e0b70),
                                   ("RVA_DEPOTICON_CLASS_CALL", "DEPOTICON_CLASS_EXPECT", 0x5e2b70, 0x5e2dc0)):
    site = const(cname); exp = byte_array(ename)
    got = pe.get_data(site, 5)
    assert got == exp and got[0] == 0xE8, f"{cname}: {got.hex(' ')} != {exp.hex(' ')}"
    assert site + 5 + struct.unpack("<i", got[1:5])[0] == addclass, f"{cname} does not call addStyleClass"
    assert fn_lo <= site < fn_hi, f"{cname} outside its function"
    # rcx = rbx (the icon component) right before the call
    prev = pe.get_data(site - 3, 3)
    assert prev == bytes([0x48, 0x8B, 0xCB]), f"{cname}: expected mov rcx,rbx before the call, got {prev.hex(' ')}"
    # only one addStyleClass call in that function, so the class lands on the icon and nothing else
    n = 0
    for j in range(fn_lo, fn_hi - 5):
        if code[j - base] == 0xE8 and j + 5 + struct.unpack_from("<i", code, j - base + 1)[0] == addclass: n += 1
    assert n == 1, f"{cname}: {n} addStyleClass calls in the function, expected 1"

# ---- the StationItem constructor pre-hook (the 1000 ms cargo-state rebuild path) ----
ctor = const("RVA_STNITEM_CTOR")
cpro = byte_array("STNITEM_CTOR_PROLOGUE")
arg = const("STNITEM_CTOR_ENTITY_ARG")
assert len(cpro) == 15 and pe.get_data(ctor, 15) == cpro, f"StationItem ctor prologue changed: {pe.get_data(ctor, 15).hex(' ')}"
cins = list(md.disasm(cpro, ctor))
assert sum(i.size for i in cins) == 15 and [i.mnemonic for i in cins] == ["mov"] * 4, [(i.mnemonic, i.op_str) for i in cins]
assert not any("rip" in i.op_str for i in cins), "a rip-relative operand in the stolen ctor prologue"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (ctor < t < ctor + 15), f"rel32 at {base + i:x} into the ctor steal"
for d in md.disasm(code[ctor - base:0x5e0b70 - base], ctor):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        assert not (ctor < d.operands[0].imm < ctor + 15), f"branch into the ctor steal at {d.address:x}"
# both callers pass the entity as the 6th argument: [rsp+0x28] at the call = [rsp+0x30] at entry
assert arg == 0x30
callers = {}
for i in range(len(code) - 5):
    if code[i] == 0xE8 and base + i + 5 + struct.unpack_from("<i", code, i + 1)[0] == ctor:
        callers[base + i] = None
assert set(callers) == {0x5dfeeb, 0x5e5a48}, f"StationItem ctor callers moved: {[hex(c) for c in callers]}"
for site, src in ((0x5dfeeb, "r14d"), (0x5e5a48, "eax")):
    window = list(md.disasm(pe.get_data(site - 0x40, 0x40), site - 0x40))
    stores = [i for i in window if i.mnemonic == "mov" and i.op_str == f"dword ptr [rsp + 0x28], {src}"]
    assert stores, f"caller {site:x}: no entity store to [rsp+0x28] from {src} before the call"
# the rebuild callback constructs the item and swaps it into the ContentView right after
after = list(md.disasm(pe.get_data(0x5e5a4d, 0x10), 0x5e5a4d))
assert after[2].mnemonic == "call" and int(after[2].op_str, 16) == 0x2286020, "rebuild path no longer calls setContent 0x2286020 after the ctor"

# ---- the post-attach restyle hook: right after DoStep hands the button to the HUD layer ----
att = const("RVA_ICON_ATTACH_HOOK")
aexp = byte_array("ICON_ATTACH_EXPECT")
assert len(aexp) == 8 and pe.get_data(att, 8) == aexp, f"attach site changed: {pe.get_data(att, 8).hex(' ')}"
ains = list(md.disasm(aexp, att))
assert [(i.mnemonic, i.op_str) for i in ains] == [("mov", "rax, qword ptr [rbp - 0x80]"), ("mov", "rbx, qword ptr [rax + 0x18]")], [(i.mnemonic, i.op_str) for i in ains]
pre = pe.get_data(att - 5, 5)
assert pre[0] == 0xE8 and (att - 5) + 5 + struct.unpack("<i", pre[1:5])[0] == 0x224a920, "the call before the attach hook is not the HUD layer add 0x224a920"
assert pe.get_data(att - 12, 3) == bytes([0x48, 0x8B, 0xD7]), "rdx is no longer the button (mov rdx, rdi) before the layer add"
assert 0x5e2dc0 <= att < 0x5e4270, "attach hook outside DoStep"
for d in md.disasm(code[0x5e2dc0 - base:0x5e4270 - base], 0x5e2dc0):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        assert not (att < d.operands[0].imm < att + 8), f"branch into the attach steal at {d.address:x}"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (att < t < att + 8), f"rel32 at {base + i:x} into the attach steal"

# ---- the engine accessor the ctor pre-hook's EnginePtr goes through ----
efp = const("RVA_ENGINE_FROM_PTR")
eins = list(md.disasm(pe.get_data(efp, 0x12), efp))
assert [(i.mnemonic, i.op_str) for i in eins[:4]] == [("sub", "rsp, 0x28"), ("call", "0x8bb7f0"), ("mov", "rax, qword ptr [rax + 0x28]"), ("add", "rsp, 0x28")], [(i.mnemonic, i.op_str) for i in eins[:4]]
vins = list(md.disasm(pe.get_data(0x8bb7f0, 0xa), 0x8bb7f0))
assert [(i.mnemonic, i.op_str) for i in vins[:3]] == [("mov", "rcx, qword ptr [rcx]"), ("mov", "rax, qword ptr [rcx]"), ("jmp", "qword ptr [rax + 8]")], "the EnginePtr virtual accessor changed"
# DoStep itself resolves the engine this way (call 0x8b9e60 then GetComponentDataIndex 0xd0920)
ds = list(md.disasm(code[0x5e2dc0 - base:0x5e4270 - base], 0x5e2dc0))
assert any(i.mnemonic == "call" and i.op_str == "0x8b9e60" for i in ds), "DoStep no longer uses the EnginePtr accessor"

# ---- the VehicleDepotItem constructor pre-hook: rcx is its EnginePtr (it hands &rcx-home to 0x8b9e60) ----
dctor = const("RVA_DEPOTITEM_CTOR")
dpro = byte_array("DEPOTITEM_CTOR_PROLOGUE")
assert len(dpro) == 9 and pe.get_data(dctor, 9) == dpro, f"depot ctor prologue changed: {pe.get_data(dctor, 9).hex(' ')}"
dins = list(md.disasm(dpro, dctor))
assert [(i.mnemonic, i.op_str) for i in dins] == [("mov", "dword ptr [rsp + 0x10], edx"), ("mov", "qword ptr [rsp + 8], rcx")], [(i.mnemonic, i.op_str) for i in dins]
head = list(md.disasm(pe.get_data(dctor, 0x48), dctor))
ops = [(i.mnemonic, i.op_str) for i in head]
assert ("lea", "rcx, [rbp + 0x67]") in ops and ("call", "0x8b9e60") in ops, "the depot ctor no longer resolves its engine from the rcx home slot"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (dctor < t < dctor + 9), f"rel32 at {base + i:x} into the depot ctor steal"
for d in md.disasm(code[dctor - base:0x5e2dc0 - base], dctor):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        assert not (dctor < d.operands[0].imm < dctor + 9), f"branch into the depot ctor steal at {d.address:x}"
assert "IconEngineNow()" in source and "g_uiEngine)" not in source.split("static int IconOwnerForEntity")[1].split("}")[0], "the owner lookup must not fall back to the cached engine"

print(f"stationicon bytes: ok -- hook {hook:x} (mov rdi,rax/xor r12d), wrap-call -> 2251620, "
      f"StationGroup ti at {ti_sg:x}, accessors present; glyph class: content-builder prologue + ctor prologue (2 callers) + 2 addStyleClass sites + post-attach hook + engine accessor + depot ctor prologue ok")
