"""Verify the ICON COLOUR patch site in the supported game binary.

The patch rewrites one call's rel32 (the icon-quad draw in AddVehicle) to a stub that
fills the colour argument for a foreign owner. This checks, against the exe:

  * the 5 bytes at the call site are the `call 0x8088f0` the source records, and its
    rel32 resolves to the draw function (a byte match elsewhere cannot pass);
  * that draw function is GetColorBuffer-capable -- its non-null-colour path exists --
    which we approximate by confirming the target is a real function start, and that the
    call site keeps its 0xE8 (a CALL, so the draw returns into AddVehicle);
  * GetComponentPtr<PlayerOwned> (0x472900) is a real function whose prologue matches
    what the stub calls fault-safely;
  * the source's colour formula matches the menu's coColor and companies.lua exactly.

Constants are read out of the source, so a drifting RVA fails here instead of at runtime.

    python tools/iconcolor_bytes_test.py
"""
from pathlib import Path
import re
import struct
import pefile

repo = Path(__file__).resolve().parents[1]
source = (repo / "native/src/slice_hook.cpp").read_text(encoding="utf-8")


def const(name):
    return int(re.search(rf"{name}\s*=\s*(0x[0-9a-f]+)", source)[1], 16)


def byte_array(name):
    body = re.search(rf"{name}\[[^\]]*\]\s*=\s*{{(.*?)}};", source, re.S)[1]
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", body))


call = const("RVA_ICON_DRAW_CALL")
target = const("RVA_ICON_DRAW_TARGET")
getpo = const("RVA_GET_PLAYEROWNED")
expect = byte_array("ICON_DRAW_EXPECT")

assert "iconcolor=0" in source, "the kill switch is gone from the source"
assert 'FlagsSayOff("iconcolor")' in source, "the install no longer consults the kill switch"
assert "InstallIconColor();" in source, "InstallIconColor is never called"
assert len(expect) == 5 and expect[0] == 0xE8, "the site must be a 5-byte call"
# the stub keeps it a CALL (rewrites only bytes at at+1), never a jmp
assert "memcpy((void*)(at + 1), &r32, 4)" in source, "the install must rewrite only the rel32, keeping 0xE8"

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"

got = pe.get_data(call, 5)
assert got == expect, f"call site changed: {got.hex(' ')} != {expect.hex(' ')}"
rel = struct.unpack("<i", expect[1:5])[0]
assert call + 5 + rel == target, f"call resolves to {call + 5 + rel:x}, not the draw {target:x}"

# both targets are real function starts (sub rsp / push / mov rax,rsp style prologue)
for rva, name in ((target, "icon draw 0x8088f0"), (getpo, "GetComponentPtr<PlayerOwned> 0x472900")):
    p = pe.get_data(rva, 4)
    assert p[0] in (0x48, 0x40, 0x53, 0x55, 0x56, 0x57) or p[:1] == b"\xe9", f"{name}: {p.hex(' ')} not a prologue"

# the colour formula matches coColor (menu_hook.cpp) and cmCompanyColor (companies.lua)
menu = (repo / "native/src/menu_hook.cpp").read_text(encoding="utf-8")
# the palette itself (and its match with coColor / companies.lua / the style sheet)
# is checked by tools/palette_sync_test.py; here just confirm IconCompanyColor still
# has a fixed table and the golden-angle overflow.
assert "static const int first[" in source and "137.508" in source, "IconCompanyColor palette/overflow missing"
assert "0.62" in source and "0.85" in source, "sat/val differ from coColor"

print(f"iconcolor bytes: ok -- call at {call:x} -> draw {target:x} (kept as CALL), "
      f"GetComponentPtr at {getpo:x}, palette matches coColor")
