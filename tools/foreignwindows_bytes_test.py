"""Verify the FOREIGN WINDOWS gate in the supported game binary.

The patch NOPs the owner `jne` in UI::ViewCreator::CanCreateView so a foreign entity's
info window is allowed to build (read-only). This checks, against the exe:

  * the two bytes before the jne are `cmp [rax],edx` (owner vs local player) as the
    source records;
  * the jne is a 6-byte near jump whose target is the function's `return 0` early-out
    (no window), and the fall-through proceeds to the type cascade;
  * nothing branches into the 6 bytes being NOPed.

The read-only guarantee itself is in inject.lua (CM.injForeignEdit) and is covered by
company_perms_test / a Lua check, not here.

    python tools/foreignwindows_bytes_test.py
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


jne = const("RVA_FOREIGNWIN_JNE")
before = byte_array("FOREIGNWIN_BEFORE")
jbytes = byte_array("FOREIGNWIN_JNE_BYTES")

assert "foreignwindows=0" in source, "the kill switch is gone from the source"
assert 'FlagsSayOff("foreignwindows")' in source, "the install no longer consults the kill switch"
assert "InstallForeignWindows();" in source, "InstallForeignWindows is never called"
assert len(jbytes) == 6 and jbytes[0] == 0x0F and jbytes[1] == 0x85, "not a 6-byte jne"

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"

got_before = pe.get_data(jne - 2, 2)
assert got_before == before, f"owner compare before is {got_before.hex(' ')}, source says {before.hex(' ')}"
got = pe.get_data(jne, 6)
assert got == jbytes, f"jne is {got.hex(' ')}, source says {jbytes.hex(' ')}"

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
ins = next(md.disasm(got, jne))
assert ins.mnemonic == "jne", f"not a jne: {ins.mnemonic}"
reject = ins.operands[0].imm

# the reject target must be a return-0 tail: xor al,al (32 c0) somewhere in the next few bytes
tail = pe.get_data(reject, 8)
assert tail[:2] == b"\x32\xc0" or b"\x32\xc0" in tail[:6], f"reject target {reject:x} is not a return-0 tail: {tail.hex(' ')}"

# nothing in the function branches into the 6 NOPed bytes
FSTART, FEND = 0x8b3020, 0x8b3400
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data(); base = text.VirtualAddress
into = []
for d in md.disasm(code[FSTART - base:FEND - base], FSTART):
    if d.mnemonic.startswith("j") and d.operands and d.operands[0].type == X86_OP_IMM:
        if jne < d.operands[0].imm < jne + 6:
            into.append(hex(d.address))
assert not into, f"something branches into the NOPed jne: {into}"
for i in range(len(code) - 5):
    if code[i] in (0xE8, 0xE9):
        t = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        assert not (jne < t < jne + 6), f"a rel32 at {base + i:x} lands inside the NOPed jne"

print(f"foreignwindows bytes: ok -- jne at {jne:x} -> return-0 {reject:x}, cmp [rax],edx intact, nothing branches in")
