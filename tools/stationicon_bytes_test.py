"""Verify the STATION ICON COLOUR hook in the supported binary.

A post-call detour at 0x5e38d0 (right after DoStep's FUN_5e45d0 call) tags the HUD
station/depot icon with a company style class. Checks, against the exe:

  * the hook site's 6 bytes are `xor r9d,r9d ; mov r8b,1` as the source records, so a
    5-byte jmp + 1 NOP replaces them and the two instructions are re-run in the stub;
  * the immediately preceding call resolves to FUN_5e45d0 (so rax at the hook is that
    call's return, the item component);
  * nothing branches into the 6 stolen bytes;
  * the StationGroup type_info string is at RVA_TI_STATIONGROUP + 0x10;
  * the accessors (type index, GetComponentPtr<StationGroup>, GetComponentPtr<PlayerOwned>)
    are real function starts.

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
get_sg = const("RVA_GET_COMPONENT_SG")
get_po = const("RVA_GET_PLAYEROWNED")

assert 'FlagsSayOff("stationicon")' in source and "InstallStationIconColor();" in source
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
assert [i.mnemonic for i in ins] == ["xor", "mov"], [(i.mnemonic, i.op_str) for i in ins]

# the call right before the hook must resolve to FUN_5e45d0 (rax = its return = component)
pre = pe.get_data(hook - 5, 5)
assert pre[0] == 0xE8, f"no call right before the hook: {pre.hex(' ')}"
callee = (hook - 5) + 5 + struct.unpack("<i", pre[1:5])[0]
assert callee == 0x5e45d0, f"pre-call resolves to {callee:x}, not FUN_5e45d0"

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
for rva, name in ((get_ti, "GetTypeIndex 0xd0a40"), (get_sg, "GetComponentPtr<StationGroup> 0x149290"),
                  (get_po, "GetComponentPtr<PlayerOwned> 0x472900")):
    p = pe.get_data(rva, 4)
    assert p[0] in (0x48, 0x4c, 0x40, 0x53, 0x55, 0x56, 0x57), f"{name} prologue {p.hex(' ')}"

print(f"stationicon bytes: ok -- hook {hook:x} (xor r9d/mov r8b), pre-call -> 5e45d0, "
      f"StationGroup ti at {ti_sg:x}, accessors present")
