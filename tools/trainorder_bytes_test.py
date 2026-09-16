"""Verify the train reservation-order hook site in the supported game binary.

The offline half of this patch is tools/trainorder_test.cpp (the ordering rule).
This half checks the things that test cannot: that the bytes slice_hook.cpp is
about to overwrite are still the bytes they were measured on, that its RVAs
point where the comments say they do, and that the stretch of engine code the
patch jumps OVER cannot be reached any other way. It reads the constants out of
the source rather than restating them, so a drifting constant fails here instead
of in a running game.

    python tools/trainorder_bytes_test.py
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


hook = const("RVA_TRAINORDER_HOOK")
resume = const("RVA_TRAINORDER_RESUME")
gametime = const("RVA_GAMETIME_GET")
typeindex = const("RVA_ECS_TYPEINDEX")
ti_name = const("RVA_TI_NAME")
steal = int(re.search(r"TRAINORDER_STEAL\s*=\s*(\d+)", source)[1])
expect = byte_array("TRAINORDER_EXPECT")
expect_resume = byte_array("TRAINORDER_EXPECT_RESUME")

assert len(expect) == steal, f"TRAINORDER_EXPECT is {len(expect)} bytes, steal is {steal}"
assert resume > hook + steal, "the relay must resume PAST the engine's shuffle"

game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe")
pe = pefile.PE(str(game), fast_load=True)
assert pe.FILE_HEADER.TimeDateStamp == 0x675ABCC6, "not the build these RVAs were measured on"

got = pe.get_data(hook, steal)
assert got == expect, f"hook site changed: {got.hex(' ')} != {expect.hex(' ')}"
assert pe.get_data(resume, len(expect_resume)) == expect_resume, "resume site moved"

# The call inside the stolen bytes is the GameTime accessor -- the same check
# InstallTrainOrder makes at run time, so a byte match that happens to land on a
# different callee cannot pass.
assert expect[8] == 0xE8, "stolen bytes no longer contain the accessor call"
rel = struct.unpack("<i", expect[9:13])[0]
assert hook + 13 + rel == gametime, f"call resolves to {hook + 13 + rel:x}, not {gametime:x}"

# ...and that accessor really is the two-line `GetComponent(...)->+0x30` getter.
acc = pe.get_data(gametime, 0x15)
assert acc[:4] == b"\x48\x83\xec\x28", "accessor prologue changed"
assert acc[0x11:0x14] == b"\x8b\x40\x30", "accessor no longer returns the int at +0x30"

# The ECS pieces the detour uses to read a train's name: the type-index lookup
# it calls, and the type_info it asks for. The decorated name is the measurement
# -- a wrong constant here would read some other component as a std::string.
assert pe.get_data(typeindex, 3) == b"\x48\x8b\xc4", "the type-index lookup moved"
decorated = pe.get_data(ti_name + 0x10, 32).split(b"\0")[0]
assert decorated == b".?AUName@component@ecs@@", f"RVA_TI_NAME is {decorated!r}"

# Nothing may branch INTO the bytes we replace (the only way in is the engine's
# own `jge` at the end of the iota fill, which targets the FIRST stolen byte),
# and nothing OUTSIDE the stretch we skip may branch into it -- the shuffle we
# are replacing has to be unreachable once the detour is in.
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
code = text.get_data()
base = text.VirtualAddress

FUNC, FUNC_END = 0xABDC20, 0xABE800          # ecs::TrainMoveSystem::Update2
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
inside, into_skipped, entry_seen = [], [], False
for ins in md.disasm(code[FUNC - base:FUNC_END - base], FUNC):
    if not ins.mnemonic.startswith("j") and ins.mnemonic != "call":
        continue
    if not ins.operands or ins.operands[0].type != X86_OP_IMM:
        continue
    tgt = ins.operands[0].imm
    if tgt == hook:
        entry_seen = True
    elif hook < tgt < hook + steal:
        inside.append((ins.address, tgt))
    elif hook + steal <= tgt < resume and not (hook + steal <= ins.address < resume):
        into_skipped.append((ins.address, tgt))
assert entry_seen, "nothing jumps to the hook site any more -- is this still the iota fill?"
assert not into_skipped, f"the skipped shuffle is still reachable: {[(hex(a), hex(b)) for a, b in into_skipped]}"

# Anything from outside the function would be a call/jmp rel32; those carry a
# 4-byte displacement, so a byte scan for them barely misfires. (Short jumps are
# left to the decode above: a byte scan for `eb`/`7x` finds them inside longer
# instructions -- the accessor call's own rel32 reads as `jl -1`.)
for i in range(len(code) - 5):
    if code[i] not in (0xE8, 0xE9):
        continue
    tgt = base + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
    if hook < tgt < resume:
        inside.append((base + i, tgt))
assert not inside, f"something branches into the patched range: {[(hex(a), hex(b)) for a, b in inside]}"

print(f"PASS: {steal} bytes at {hook:x} are the measured sequence, the call resolves to the "
      f"GameTime accessor at {gametime:x}, the Name type_info at {ti_name:x} is "
      f"{decorated.decode()}, the relay resumes at {resume:x}, and nothing branches into "
      f"the {resume - hook} bytes the patch replaces")
