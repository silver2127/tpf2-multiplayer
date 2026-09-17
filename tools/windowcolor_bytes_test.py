"""Verify the WINDOW COLOUR and STATION LABEL COLOUR sites in the supported binary.

WINDOW COLOUR: a near-jump detour on the entity-window bind 0x8b2390 (steal the
2-instr, 9-byte prologue) tags a foreign window with a company style class.
STATION LABEL COLOUR: rewrites the rel32 of the label-background draw call at
0x80a0ee to a stub that overrides its colour for a foreign station.

Checks, against the exe:
  * the window-bind prologue is `push rbx ; sub rsp,0x80` (9 bytes) as the source
    records, so a 5-byte near jmp + 9-byte trampoline is valid, and nothing branches
    into those 9 bytes;
  * the station-label call site is `call 0x8090f0` and its rel32 resolves there;
  * the addStyleClass target 0x227a1e0 is a real function;
  * the style sheet defines !mpWinCoN.

    python tools/windowcolor_bytes_test.py
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


bind = const("RVA_WINDOW_BIND")
bind_expect = byte_array("WINDOW_BIND_EXPECT")
addclass = const("RVA_ADD_STYLE_CLASS")
stn = const("RVA_STNLABEL_CALL")
stn_t = const("RVA_STNLABEL_TARGET")
stn_expect = byte_array("STNLABEL_EXPECT")

for flag in ("windowcolor", "iconcolor"):
    assert f'FlagsSayOff("{flag}")' in source
assert "InstallWindowColor();" in source and "InstallStationLabelColor();" in source
assert "memcpy((void*)(at + 1), &r32, 4)" in source  # station label keeps the CALL

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the measured build"
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b"\x2etext" or s.Name.rstrip(b"\0") == b".text")
code = text.get_data(); base = text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

# window bind prologue
got = pe.get_data(bind, len(bind_expect))
assert got == bind_expect, f"window bind prologue changed: {got.hex(' ')} != {bind_expect.hex(' ')}"
# the 9 bytes are exactly 2 whole instructions (push rbx ; sub rsp,0x80)
ins = list(md.disasm(got, bind))
assert len(ins) == 2 and ins[0].mnemonic == "push" and ins[1].mnemonic == "sub", [(i.mnemonic, i.op_str) for i in ins]
assert ins[0].size + ins[1].size == 9
# nothing branches into the 9 stolen bytes
for d in md.disasm(code[0x8b2000 - base:0x8b2800 - base], 0x8b2000):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        assert not (bind < d.operands[0].imm < bind + 9), f"branch into window-bind steal at {d.address:x}"
# addStyleClass is a real function prologue
p = pe.get_data(addclass, 4)
assert p[0] in (0x48, 0x4c, 0x40, 0x53, 0x55, 0x56, 0x57), f"addStyleClass prologue {p.hex(' ')}"

# station label call
got = pe.get_data(stn, 5)
assert got == stn_expect, f"station label call changed: {got.hex(' ')}"
assert got[0] == 0xE8
rel = struct.unpack("<i", got[1:5])[0]
assert stn + 5 + rel == stn_t, f"station label call resolves to {stn + 5 + rel:x}, not {stn_t:x}"

# style sheet class
ss = (repo / "mod/mp_lockstep_1/res/config/style_sheet/mp_lockstep.lua").read_text(encoding="utf-8")
assert '"!mpWinCo"' in ss and "0.30" in ss, "style sheet missing the !mpWinCoN translucent class"

print(f"windowcolor bytes: ok -- bind {bind:x} (9-byte push+sub prologue), station label call {stn:x} -> {stn_t:x}, !mpWinCoN defined")
